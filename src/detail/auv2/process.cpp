#include "process.h"
#include "detail/shared/midi_translation.h"

#include <algorithm>
#include <cmath>
#include <cassert>
#include <cstring>

namespace Clap::AUv2
{

inline clap_beattime doubleToBeatTime(double t)
{
  return std::round(t * CLAP_BEATTIME_FACTOR);
}

inline clap_sectime doubleToSecTime(double t)
{
  return round(t * CLAP_SECTIME_FACTOR);
}

ProcessAdapter::~ProcessAdapter()
{
  if (_input_ports)
  {
    for (uint32_t i = 0; i < _numInputs; ++i)
    {
      delete[] _input_ports[i].data32;
    }
    delete[] _input_ports;
    _input_ports = nullptr;
  }
  if (_output_ports)
  {
    for (uint32_t i = 0; i < _numOutputs; ++i)
    {
      delete[] _output_ports[i].data32;
    }
    delete[] _output_ports;
    _output_ports = nullptr;
  }
}

void ProcessAdapter::setupProcessing(ausdk::AUScope &audioInputs, ausdk::AUScope &audioOutputs,
                                     const clap_plugin_t *plugin, const clap_plugin_params_t *ext_params,
                                     Clap::IAutomation *automationInterface, ParameterTree *parameters,
                                     IMIDIOutputs *midiouts, uint32_t numMaxSamples,
                                     uint32_t preferredMIDIDialect, uint32_t supportedMIDIDialects,
                                     uint32_t clapAudioInputs, uint32_t clapAudioOutputs)
{
  _plugin = plugin;
  _ext_params = ext_params;
  _automation = automationInterface;
  _parameters = parameters;

  _supported_midi_dialects = supportedMIDIDialects;
  _preferred_midi_dialect =
      ClapWrapper::detail::shared::chooseInputDialect(preferredMIDIDialect, supportedMIDIDialects);

  _midiouts = midiouts;

  // rewrite the buffer structures
  _audioInputScope = &audioInputs;
  _audioOutputScope = &audioOutputs;

  // setup silent streaming
  if (numMaxSamples > 0)
  {
    delete[] _silent_input;
    _silent_input = new float[numMaxSamples];

    delete[] _silent_output;
    _silent_output = new float[numMaxSamples];
  }

  _numInputs = _audioInputScope->GetNumberOfElements();
  _numOutputs = _audioOutputScope->GetNumberOfElements();

  // Never hand the plugin more ports than _input_ports/_output_ports have
  // elements: the AU element counts were fixed at PostConstructor, while the
  // CLAP counts are re-queried on every (re)activation — a plugin that rescans
  // its audio ports while deactivated could otherwise make process() index past
  // the allocation, or receive a null pointer with a non-zero count.
  _clapNumInputs = std::min(clapAudioInputs, _numInputs);
  _clapNumOutputs = std::min(clapAudioOutputs, _numOutputs);

  // The plugin is handed its own declared port counts, which may be fewer than
  // the AU-scope element counts (a note-only plugin gets a placeholder silent AU
  // output bus but zero CLAP audio ports).
  _processData.audio_inputs_count = _clapNumInputs;
  delete[] _input_ports;
  _input_ports = nullptr;

  if (_numInputs > 0)
  {
    _input_ports = new clap_audio_buffer_t[_numInputs];
    for (auto i = 0U; i < _numInputs; ++i)
    {
      clap_audio_buffer_t &bus = _input_ports[i];
      auto &info = static_cast<ausdk::AUInputElement &>(*_audioInputScope->SafeGetElement(i));
      {
        bus.channel_count = info.NumberChannels();
        bus.constant_mask = 0;
        bus.latency = 0;
        bus.data64 = nullptr;
        bus.data32 = new float *[info.NumberChannels()];
      }
    }
    _processData.audio_inputs = _input_ports;
  }
  else
  {
    _processData.audio_inputs = nullptr;
  }

  _processData.audio_outputs_count = _clapNumOutputs;
  delete[] _output_ports;
  _output_ports = nullptr;

  if (_numOutputs > 0)
  {
    _output_ports = new clap_audio_buffer_t[_numOutputs];
    for (auto i = 0U; i < _numOutputs; ++i)
    {
      clap_audio_buffer_t &bus = _output_ports[i];

      auto &info = static_cast<ausdk::AUOutputElement &>(*_audioOutputScope->SafeGetElement(i));
      {
        bus.channel_count = info.NumberChannels();
        bus.constant_mask = 0;
        bus.latency = 0;
        bus.data64 = nullptr;
        bus.data32 = new float *[info.NumberChannels()];
      }
    }
    _processData.audio_outputs = _output_ports;
  }
  else
  {
    _processData.audio_outputs = nullptr;
  }

  // wire up internal structures
  _processData.in_events = &_in_events;
  _processData.out_events = &_out_events;

  _transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  _transport.header.type = CLAP_EVENT_TRANSPORT;
  _transport.header.time = 0;
  _transport.header.size = sizeof(clap_event_transport_t);
  _processData.transport = &_transport;

  _in_events.ctx = this;
  _in_events.size = input_events_size;
  _in_events.get = input_events_get;

  _out_events.ctx = this;
  _out_events.try_push = output_events_try_push;

  _events.clear();
  _events.reserve(8192);
  _eventindices.clear();
  _eventindices.reserve(_events.capacity());
  _sysexBuffers.prepare(16);

  _out_events.ctx = this;

  _gesturedParameters.reserve(8192);

  _activeNotes.reserve(32);
}

void ProcessAdapter::sortEventIndices()
{
  // just sorting the index
  // an item must be sorted to front of
  // if the timestamp if event[a] is earlier than
  // the timestamp of event[b].
  // if they have the same timestamp, the index must be preserved

  std::sort(_eventindices.begin(), _eventindices.end(),
            [&](size_t const &a, size_t const &b)
            {
              auto t1 = _events[a].header.time;
              auto t2 = _events[b].header.time;
              return (t1 == t2) ? (a < b) : (t1 < t2);
            });
}

void ProcessAdapter::process(ProcessData &data)
{
  // CLAP requires event times within [0, frames_count); a host stamping
  // MIDIEventList packets with out-of-range timestamps (e.g. mach host time
  // instead of sample offsets) would otherwise make plugins that index their
  // buffers by event time read out of bounds — clamp into the block.
  if (data.numSamples > 0)
  {
    for (auto &e : _events)
    {
      if (e.header.time >= data.numSamples)
      {
        e.header.time = data.numSamples - 1;
      }
    }
  }
  sortEventIndices();
  _processData.frames_count = data.numSamples;
  _transport.flags = 0;

  if (data._AUtransportValid)
  {
    // TODO: transportchanged flag?
    _transport.flags |= data._isPlaying ? CLAP_TRANSPORT_IS_PLAYING : 0;
    _transport.flags |= data._isRecording ? CLAP_TRANSPORT_IS_RECORDING : 0;
    // CLAP_TRANSPORT_IS_RECORDING can not be retrieved from this data block
    _transport.flags |= data._isLooping ? CLAP_TRANSPORT_IS_LOOP_ACTIVE : 0;
    // CLAP_TRANSPORT_IS_RECORDING can not be retrieved from the AudioUnit API
    _transport.loop_end_beats = data._cycleStart;
    _transport.loop_end_beats = data._cycleEnd;

    _transport.song_pos_seconds = doubleToSecTime(data._currentSongPosInSeconds);
    _transport.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
  }
  if (data._AUbeatAndTempoValid)
  {
    if (data._tempo != 0.0)
    {
      _transport.tempo = data._tempo;
      _transport.flags |= CLAP_TRANSPORT_HAS_TEMPO;
    }
    if (data._beat)
    {
      _transport.song_pos_beats = doubleToBeatTime(data._beat);
      _transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
    }
  }
  if (data._AUmusicalTimeValid)
  {
    _transport.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
    _transport.tsig_denom = data._musicalDenominator;
    _transport.tsig_num = data._musicalNumerator;
    _transport.bar_start = data._currentDownBeat * CLAP_BEATTIME_FACTOR;
  }

  if (_numInputs)
  {
    for (uint32_t i = 0; i < _numInputs; ++i)
    {
      auto &m = static_cast<ausdk::AUInputElement &>(*_audioInputScope->SafeGetElement(i));
      if (m.PullInput(data.flags, data.timestamp, i, data.numSamples) == noErr)
      {
        AudioBufferList &myInBuffers = m.GetBufferList();
        auto num = myInBuffers.mNumberBuffers;
        this->_input_ports[i].channel_count = num;
        for (uint32_t j = 0; j < num; ++j)
        {
          assert(myInBuffers.mBuffers[j].mNumberChannels == 1);
          this->_input_ports[i].data32[j] = (float *)myInBuffers.mBuffers[j].mData;
        }
        // _input_ports[0].data32 = myInBuffers[0].mData;
      }
      else
      {
        for (uint32_t j = 0; j < this->_input_ports[i].channel_count; ++j)
        {
          this->_input_ports[i].data32[j] = _silent_input;
        }
      }
    }
  }
#if 0
  // older code template: remove
  // input handlling
  if ( _input_ports->channel_count() > 0)

  {
    _input_ports-
    auto* inp = *(_input_ports)(0);
    
    if ( GetInput(0)->PullInput(inFlags, inTimeStamp, 0, inFrames) == noErr )
    {
      AudioBufferList  &myInBuffers = GetInput(0)->GetBufferList();
      mInputs = (float *) myInBuffers.mBuffers[i].mData;
    }
  }
#endif
#if 1
  // output handling
  for (uint32_t i = 0; i < _numOutputs; i++)
  {
    auto &m = static_cast<ausdk::AUOutputElement &>(*_audioOutputScope->SafeGetElement(i));
    AudioBufferList &myOutBuffers = m.PrepareBuffer(data.numSamples);
    auto num = myOutBuffers.mNumberBuffers;
    this->_output_ports[i].channel_count = num;
    for (uint32_t j = 0; j < num; ++j)
    {
      assert(myOutBuffers.mBuffers[j].mNumberChannels == 1);
      this->_output_ports[i].data32[j] = (float *)myOutBuffers.mBuffers[j].mData;
      // placeholder AU output busses beyond the plugin's declared CLAP ports are
      // not written by the plugin, so emit silence rather than leaving them stale.
      if (i >= _clapNumOutputs)
      {
        std::memset(this->_output_ports[i].data32[j], 0, sizeof(float) * data.numSamples);
      }
    }
  }
#endif

  _plugin->process(_plugin, &_processData);

  processOutputEvents();

  // clean up and prepare the events for the next cycle
  _events.clear();
  _eventindices.clear();
  _sysexBuffers.reset();
}

uint32_t ProcessAdapter::input_events_size(const struct clap_input_events *list)
{
  auto self = static_cast<ProcessAdapter *>(list->ctx);
  auto k = (uint32_t)self->_events.size();
  return k;
  // return self->_vstdata->inputEvents->getEventCount();
}

// returns the pointer to an event in the list. The index accessed is not the position in the event list itself
// since all events indices were sorted by timestamp
const clap_event_header_t *ProcessAdapter::input_events_get(const struct clap_input_events *list,
                                                            uint32_t index)
{
  auto self = static_cast<ProcessAdapter *>(list->ctx);
  if (self->_events.size() > index)
  {
    // we can safely return the note.header also for other event types
    // since they are at the same memory address
    auto realindex = self->_eventindices[index];
    return &(self->_events[realindex].header);
  }
  return nullptr;
}

bool ProcessAdapter::output_events_try_push(const struct clap_output_events *list,
                                            const clap_event_header_t *event)
{
  auto self = static_cast<ProcessAdapter *>(list->ctx);
  // mainly used for CLAP_EVENT_NOTE_CHOKE and CLAP_EVENT_NOTE_END
  // but also for parameter changes
  return self->enqueueOutputEvent(event);
}

bool ProcessAdapter::enqueueOutputEvent(const clap_event_header_t *event)
{
  switch (event->type)
  {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_MIDI:
    {
      auto nevt = reinterpret_cast<const clap_multi_event_t *>(event);
      _midiouts->send(*nevt);
      return true;
    }
    case CLAP_EVENT_NOTE_END:
    case CLAP_EVENT_NOTE_CHOKE:
      removeFromActiveNotes((const clap_event_note *)(event));
      return true;
      break;
    case CLAP_EVENT_NOTE_EXPRESSION:
    {
      auto nevt = reinterpret_cast<const clap_multi_event_t *>(event);
      _midiouts->send(*nevt);
      return true;
    }
    break;
    case CLAP_EVENT_PARAM_VALUE:
    {
      auto ev = (clap_event_param_value *)event;
      _automation->onPerformEdit(ev);
    }

      return true;
      break;
    case CLAP_EVENT_PARAM_MOD:
      return true;
      break;
    case CLAP_EVENT_PARAM_GESTURE_BEGIN:
    {
      auto ev = (clap_event_param_gesture *)event;
      auto param = _parameters->find(ev->param_id);
      if (param != _parameters->end())
      {
        _gesturedParameters.push_back(ev->param_id);
        _automation->onBeginEdit(ev->param_id);
      }
    }
      return true;

      break;
    case CLAP_EVENT_PARAM_GESTURE_END:
    {
      auto ev = (clap_event_param_gesture *)event;
      auto n = std::remove(_gesturedParameters.begin(), _gesturedParameters.end(), ev->param_id);
      if (n != _gesturedParameters.end())
      {
        _gesturedParameters.erase(n, _gesturedParameters.end());
        _automation->onEndEdit(ev->param_id);
      }
    }
      return true;
      break;

    case CLAP_EVENT_MIDI_SYSEX:
    {
      auto nevt = reinterpret_cast<const clap_multi_event_t *>(event);
      _midiouts->send(*nevt);
      return true;
    }
    break;
    case CLAP_EVENT_MIDI2:
    {
      auto nevt = reinterpret_cast<const clap_multi_event_t *>(event);
      _midiouts->send(*nevt);
      return true;
    }
    break;
    default:
      break;
  }
  return false;
}

void ProcessAdapter::addToActiveNotes(const clap_event_note *note)
{
  for (auto &i : _activeNotes)
  {
    if (!i.used)
    {
      i.note_id = note->note_id;
      i.port_index = note->port_index;
      i.channel = note->channel;
      i.key = note->key;
      i.used = true;
      return;
    }
  }
  _activeNotes.emplace_back(ActiveNote{true, note->note_id, note->port_index, note->channel, note->key});
}

void ProcessAdapter::removeFromActiveNotes(const clap_event_note *note)
{
  for (auto &i : _activeNotes)
  {
    if (i.used && i.port_index == note->port_index && i.channel == note->channel &&
        i.note_id == note->note_id)
    {
      i.used = false;
    }
  }
}

void ProcessAdapter::processOutputEvents()
{
}

void ProcessAdapter::addMIDIEvent(UInt32 inStatus, UInt32 inData1, UInt32 inData2,
                                  UInt32 inOffsetSampleFrame)
{
  const UInt32 strippedStatus = (inStatus & 0xf0U) >> 4;  // NOLINT
  const UInt32 channel = inStatus & 0x0fU;                // NOLINT

  auto deltaFrames = inOffsetSampleFrame & kMusicDeviceSampleFrameMask_SampleOffset;

  bool live = (inOffsetSampleFrame & kMusicDeviceSampleFrameMask_IsScheduled) != 0;

  clap_multi_event n;
  n.header.time = deltaFrames;
  // type is being set further down
  n.header.flags = 0 + (live ? CLAP_EVENT_IS_LIVE : 0);
  // n.header.size is set further down
  n.header.space_id = 0;

  switch (strippedStatus)
  {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    default:
      // no running status
      break;
    case 8:  // note off
    case 9:  // note on
    {
      // MIDI 1.0 running-status convention: a note-on with velocity 0 is a note-off
      const bool noteOn = (strippedStatus == 9) && ((inData2 & 0x7F) != 0);
      switch (_preferred_midi_dialect)
      {
        case CLAP_NOTE_DIALECT_CLAP:
          n.header.type = noteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
          n.header.size = sizeof(clap_event_note_t);

          n.note.port_index = 0;
          n.note.note_id = -1;
          n.note.key = (inData1 & 0x7F);
          n.note.velocity = 1.f * (inData2 & 0x7F) / 127.f;
          n.note.channel = channel;
          break;
        case CLAP_NOTE_DIALECT_MIDI:
        case CLAP_NOTE_DIALECT_MIDI_MPE:
        case CLAP_NOTE_DIALECT_MIDI2:
        default:
          // A legacy MIDI1 source note maps to raw MIDI for every non-CLAP
          // dialect. (Synthesising MIDI2/UMP from a MIDI1 source, when a plugin
          // prefers MIDI2, is handled on the UMP path, not here.)
          n.header.type = CLAP_EVENT_MIDI;
          n.header.size = sizeof(clap_event_midi_t);

          n.midi.port_index = 0;
          n.midi.data[0] = inStatus;
          n.midi.data[1] = inData1;
          n.midi.data[2] = inData2;
          break;
      }
      this->_eventindices.emplace_back((this->_events.size()));
      this->_events.emplace_back(n);
      // Active-note bookkeeping backs note-expression translation and is only
      // meaningful for typed CLAP notes; the n.note union fields are only valid
      // when the CLAP branch above populated them.
      if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
      {
        if (noteOn)
          addToActiveNotes(&n.note);
        else
          removeFromActiveNotes(&n.note);
      }
      break;
    }
    case 0xA:  // polyphonic (per-key) pressure / aftertouch
      if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
      {
        // translate to a per-note pressure expression. note_id is a wildcard
        // (-1) because notes created on the MIDI input path have no id; the
        // key+channel identify the target note.
        n.header.type = CLAP_EVENT_NOTE_EXPRESSION;
        n.header.size = sizeof(clap_event_note_expression_t);

        n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
        n.noteexpression.note_id = -1;
        n.noteexpression.port_index = 0;
        n.noteexpression.channel = channel;
        n.noteexpression.key = (inData1 & 0x7F);
        n.noteexpression.value = 1.0 * (inData2 & 0x7F) / 127.0;  // range 0..1
      }
      else
      {
        n.header.type = CLAP_EVENT_MIDI;
        n.header.size = sizeof(clap_event_midi_t);

        n.midi.port_index = 0;
        n.midi.data[0] = inStatus;
        n.midi.data[1] = inData1;
        n.midi.data[2] = inData2;
      }
      this->_eventindices.emplace_back((this->_events.size()));
      this->_events.emplace_back(n);
      break;
    case 0xD:  // channel pressure (2 bytes) -> channel-wide pressure expression
      if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
      {
        n.header.type = CLAP_EVENT_NOTE_EXPRESSION;
        n.header.size = sizeof(clap_event_note_expression_t);
        n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
        n.noteexpression.note_id = -1;
        n.noteexpression.port_index = 0;
        n.noteexpression.channel = channel;
        n.noteexpression.key = -1;                                // channel-wide (wildcard key)
        n.noteexpression.value = 1.0 * (inData1 & 0x7F) / 127.0;  // range 0..1
      }
      else
      {
        n.header.type = CLAP_EVENT_MIDI;
        n.header.size = sizeof(clap_event_midi_t);
        n.midi.port_index = 0;
        n.midi.data[0] = inStatus;
        n.midi.data[1] = inData1;
        n.midi.data[2] = inData2;
      }
      this->_eventindices.emplace_back((this->_events.size()));
      this->_events.emplace_back(n);
      break;
    case 0xE:  // pitch bend -> channel-wide tuning expression (+/- 2 semitones)
      if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
      {
        const int bend14 = ((inData2 & 0x7F) << 7) | (inData1 & 0x7F);
        n.header.type = CLAP_EVENT_NOTE_EXPRESSION;
        n.header.size = sizeof(clap_event_note_expression_t);
        n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
        n.noteexpression.note_id = -1;
        n.noteexpression.port_index = 0;
        n.noteexpression.channel = channel;
        n.noteexpression.key = -1;                                // channel-wide (wildcard key)
        n.noteexpression.value = (bend14 - 8192) / 8192.0 * 2.0;  // semitones
      }
      else
      {
        n.header.type = CLAP_EVENT_MIDI;
        n.header.size = sizeof(clap_event_midi_t);
        n.midi.port_index = 0;
        n.midi.data[0] = inStatus;
        n.midi.data[1] = inData1;
        n.midi.data[2] = inData2;
      }
      this->_eventindices.emplace_back((this->_events.size()));
      this->_events.emplace_back(n);
      break;
    case 0xB:  // control change
    case 0xC:  // program change (2 bytes)
      // CLAP has no generic typed event for these; forward as raw MIDI.
      n.header.type = CLAP_EVENT_MIDI;
      n.header.size = sizeof(clap_event_midi_t);

      n.midi.port_index = 0;
      n.midi.data[0] = inStatus;
      n.midi.data[1] = inData1;
      n.midi.data[2] = inData2;

      this->_eventindices.emplace_back((this->_events.size()));
      this->_events.emplace_back(n);
      break;
    case 0xF:
      break;
  }
}

void ProcessAdapter::addMIDI2Event(const uint32_t *words, uint32_t nWords, UInt32 inOffsetSampleFrame)
{
  if (!words || nWords == 0) return;

  auto deltaFrames = inOffsetSampleFrame & kMusicDeviceSampleFrameMask_SampleOffset;
  bool live = (inOffsetSampleFrame & kMusicDeviceSampleFrameMask_IsScheduled) != 0;

  clap_multi_event n;
  n.header.time = deltaFrames;
  n.header.type = CLAP_EVENT_MIDI2;
  n.header.size = sizeof(clap_event_midi2_t);
  n.header.flags = 0 + (live ? CLAP_EVENT_IS_LIVE : 0);
  n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;

  n.midi2.port_index = 0;
  n.midi2.data[0] = words[0];
  n.midi2.data[1] = (nWords > 1) ? words[1] : 0;
  n.midi2.data[2] = (nWords > 2) ? words[2] : 0;
  n.midi2.data[3] = (nWords > 3) ? words[3] : 0;

  this->_eventindices.emplace_back(this->_events.size());
  this->_events.emplace_back(n);
}

void ProcessAdapter::addSysExEvent(const uint8_t *data, uint32_t length, UInt32 inOffsetSampleFrame)
{
  if (!data || length == 0) return;

  auto deltaFrames = inOffsetSampleFrame & kMusicDeviceSampleFrameMask_SampleOffset;
  bool live = (inOffsetSampleFrame & kMusicDeviceSampleFrameMask_IsScheduled) != 0;

  // keep the payload alive until the plugin consumes it during process()
  const auto &owned = _sysexBuffers.acquire(data, length);

  clap_multi_event n;
  n.header.time = deltaFrames;
  n.header.type = CLAP_EVENT_MIDI_SYSEX;
  n.header.size = sizeof(clap_event_midi_sysex_t);
  n.header.flags = 0 + (live ? CLAP_EVENT_IS_LIVE : 0);
  n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;

  n.sysex.port_index = 0;
  n.sysex.buffer = owned.data();
  n.sysex.size = static_cast<uint32_t>(owned.size());

  this->_eventindices.emplace_back(this->_events.size());
  this->_events.emplace_back(n);
}

void ProcessAdapter::startNote(int32_t note_id, int16_t channel, float pitch, float velocity,
                               UInt32 inOffsetSampleFrame)
{
  auto deltaFrames = inOffsetSampleFrame & kMusicDeviceSampleFrameMask_SampleOffset;
  bool live = (inOffsetSampleFrame & kMusicDeviceSampleFrameMask_IsScheduled) != 0;
  const int16_t key = static_cast<int16_t>(note_id & 0x7F);

  clap_multi_event n;
  n.header.time = deltaFrames;
  n.header.flags = 0 + (live ? CLAP_EVENT_IS_LIVE : 0);
  n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;

  if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
  {
    n.header.type = CLAP_EVENT_NOTE_ON;
    n.header.size = sizeof(clap_event_note_t);
    n.note.port_index = 0;
    n.note.note_id = note_id;
    n.note.channel = channel;
    n.note.key = key;
    n.note.velocity = velocity / 127.0;
    this->_eventindices.emplace_back(this->_events.size());
    this->_events.emplace_back(n);
    addToActiveNotes(&n.note);

    // carry the fractional part of the pitch as a per-note tuning expression
    const double frac = static_cast<double>(pitch) - static_cast<double>(key);
    if (frac != 0.0)
    {
      clap_multi_event t;
      t.header.time = deltaFrames;
      t.header.flags = n.header.flags;
      t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      t.header.type = CLAP_EVENT_NOTE_EXPRESSION;
      t.header.size = sizeof(clap_event_note_expression_t);
      t.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_TUNING;  // semitones
      t.noteexpression.note_id = note_id;
      t.noteexpression.port_index = 0;
      t.noteexpression.channel = channel;
      t.noteexpression.key = key;
      t.noteexpression.value = frac;
      this->_eventindices.emplace_back(this->_events.size());
      this->_events.emplace_back(t);
    }
  }
  else
  {
    float v = velocity;
    if (v < 0.f) v = 0.f;
    if (v > 127.f) v = 127.f;
    n.header.type = CLAP_EVENT_MIDI;
    n.header.size = sizeof(clap_event_midi_t);
    n.midi.port_index = 0;
    n.midi.data[0] = static_cast<uint8_t>(0x90u | (channel & 0x0F));
    n.midi.data[1] = static_cast<uint8_t>(key & 0x7F);
    n.midi.data[2] = static_cast<uint8_t>(v);
    this->_eventindices.emplace_back(this->_events.size());
    this->_events.emplace_back(n);
  }
}

void ProcessAdapter::stopNote(int32_t note_id, int16_t channel, UInt32 inOffsetSampleFrame)
{
  auto deltaFrames = inOffsetSampleFrame & kMusicDeviceSampleFrameMask_SampleOffset;
  bool live = (inOffsetSampleFrame & kMusicDeviceSampleFrameMask_IsScheduled) != 0;
  const int16_t key = static_cast<int16_t>(note_id & 0x7F);

  clap_multi_event n;
  n.header.time = deltaFrames;
  n.header.flags = 0 + (live ? CLAP_EVENT_IS_LIVE : 0);
  n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;

  if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
  {
    n.header.type = CLAP_EVENT_NOTE_OFF;
    n.header.size = sizeof(clap_event_note_t);
    n.note.port_index = 0;
    n.note.note_id = note_id;
    n.note.channel = channel;
    n.note.key = key;
    n.note.velocity = 0.0;
    this->_eventindices.emplace_back(this->_events.size());
    this->_events.emplace_back(n);
    removeFromActiveNotes(&n.note);
  }
  else
  {
    n.header.type = CLAP_EVENT_MIDI;
    n.header.size = sizeof(clap_event_midi_t);
    n.midi.port_index = 0;
    n.midi.data[0] = static_cast<uint8_t>(0x80u | (channel & 0x0F));
    n.midi.data[1] = static_cast<uint8_t>(key & 0x7F);
    n.midi.data[2] = 0;
    this->_eventindices.emplace_back(this->_events.size());
    this->_events.emplace_back(n);
  }
}

void ProcessAdapter::addParameterEvent(const clap_param_info_t &info, double value,
                                       uint32_t inOffsetSampleFrame)
{
  clap_multi_event n;
  n.header.size = sizeof(n.param);
  n.header.type = CLAP_EVENT_PARAM_VALUE;
  n.header.space_id = 0;
  n.header.time = inOffsetSampleFrame;
  n.header.flags = 0;

  n.param.value = value;
  n.param.param_id = info.id;
  n.param.cookie = info.cookie;
  n.param.port_index = -1;
  n.param.key = -1;
  n.param.channel = -1;
  n.param.note_id = -1;

  this->_eventindices.emplace_back(this->_events.size());
  this->_events.emplace_back(n);
}
}  // namespace Clap::AUv2
