#include "process.h"

#include <algorithm>
#include <cmath>
#include "../clap/automation.h"

namespace Clap::AUv2
{

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

void ProcessAdapter::setupProcessing(ausdk::AUScope& audioInputs, ausdk::AUScope& audioOutputs,
                                     const clap_plugin_t* plugin, const clap_plugin_params_t* ext_params,
                                     uint32_t numMaxSamples)
{
  _plugin = plugin;
  _ext_params = ext_params;

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

  _processData.audio_inputs_count = _numInputs;
  delete[] _input_ports;
  _input_ports = nullptr;

  if (_numInputs > 0)
  {
    _input_ports = new clap_audio_buffer_t[_numInputs];
    for (auto i = 0U; i < _numInputs; ++i)
    {
      clap_audio_buffer_t& bus = _input_ports[i];
      auto& info = static_cast<ausdk::AUInputElement&>(*_audioInputScope->SafeGetElement(i));
      {
        bus.channel_count = info.NumberChannels();
        bus.constant_mask = 0;
        bus.latency = 0;
        bus.data64 = nullptr;
        bus.data32 = new float*[info.NumberChannels()];
      }
    }
    _processData.audio_inputs = _input_ports;
  }
  else
  {
    _processData.audio_inputs = nullptr;
  }

  _processData.audio_outputs_count = _numOutputs;
  delete[] _output_ports;
  _output_ports = nullptr;

  if (_numOutputs > 0)
  {
    _output_ports = new clap_audio_buffer_t[_numOutputs];
    for (auto i = 0U; i < _numOutputs; ++i)
    {
      clap_audio_buffer_t& bus = _output_ports[i];

      auto& info = static_cast<ausdk::AUOutputElement&>(*_audioOutputScope->SafeGetElement(i));
      {
        bus.channel_count = info.NumberChannels();
        bus.constant_mask = 0;
        bus.latency = 0;
        bus.data64 = nullptr;
        bus.data32 = new float*[info.NumberChannels()];
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
            [&](size_t const& a, size_t const& b)
            {
              auto t1 = _events[a].header.time;
              auto t2 = _events[b].header.time;
              return (t1 == t2) ? (a < b) : (t1 < t2);
            });
}

void ProcessAdapter::process(ProcessData& data)
{
  sortEventIndices();
  _processData.frames_count = data.numSamples;
  _transport.flags = 0;

  if (data._AUtransportValid)
  {
    _transport.flags += data._isPlaying ? CLAP_TRANSPORT_IS_PLAYING : 0;
    _transport.flags += data._isLooping ? CLAP_TRANSPORT_IS_LOOP_ACTIVE : 0;
    // CLAP_TRANSPORT_IS_RECORDING can not be retrieved from the AudioUnit API
    _transport.loop_start_beats = data._cycleStart;
    _transport.loop_end_beats = data._cycleEnd;
  }

  if (_numInputs)
  {
    for (uint32_t i = 0; i < _numInputs; ++i)
    {
      auto& m = static_cast<ausdk::AUInputElement&>(*_audioInputScope->SafeGetElement(0));
      if (m.PullInput(data.flags, data.timestamp, 0, data.numSamples))
      {
        AudioBufferList& myInBuffers = m.GetBufferList();
        auto num = myInBuffers.mBuffers[0].mNumberChannels;
        this->_input_ports[i].channel_count = num;
        for (uint32_t j = 0; j < num; ++j)
          this->_input_ports[i].data32[j] = (float*)myInBuffers.mBuffers[j].mData;

        // _input_ports[0].data32 = myInBuffers[0].mData;
      }
    }
  }
#if 0
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
  // long myOutBus = 0;
  // long myChannel = 0;
  for (uint32_t i = 0; i < _numOutputs; i++)
  {
    auto& m = static_cast<ausdk::AUOutputElement&>(*_audioOutputScope->SafeGetElement(0));
    AudioBufferList& myOutBuffers = m.PrepareBuffer(data.numSamples);
    auto num = myOutBuffers.mBuffers[0].mNumberChannels;
    this->_output_ports[i].channel_count = num;
    for (uint32_t j = 0; j < num; ++j)
      this->_output_ports[i].data32[j] = (float*)myOutBuffers.mBuffers[j].mData;
  }
#endif

  _plugin->process(_plugin, &_processData);

  // clean up and prepare the events for the next cycle
  _events.clear();
  _eventindices.clear();
}

uint32_t ProcessAdapter::input_events_size(const struct clap_input_events* list)
{
  auto self = static_cast<ProcessAdapter*>(list->ctx);
  auto k = (uint32_t)self->_events.size();
  return k;
  // return self->_vstdata->inputEvents->getEventCount();
}

// returns the pointer to an event in the list. The index accessed is not the position in the event list itself
// since all events indices were sorted by timestamp
const clap_event_header_t* ProcessAdapter::input_events_get(const struct clap_input_events* list,
                                                            uint32_t index)
{
  auto self = static_cast<ProcessAdapter*>(list->ctx);
  if (self->_events.size() > index)
  {
    // we can safely return the note.header also for other event types
    // since they are at the same memory address
    auto realindex = self->_eventindices[index];
    return &(self->_events[realindex].header);
  }
  return nullptr;
}

bool ProcessAdapter::output_events_try_push(const struct clap_output_events* list,
                                            const clap_event_header_t* event)
{
  auto self = static_cast<ProcessAdapter*>(list->ctx);
  // mainly used for CLAP_EVENT_NOTE_CHOKE and CLAP_EVENT_NOTE_END
  // but also for parameter changes
  return self->enqueueOutputEvent(event);
}

bool ProcessAdapter::enqueueOutputEvent(const clap_event_header_t* event)
{
  switch (event->type)
  {
    case CLAP_EVENT_NOTE_ON:
    {
      auto nevt = reinterpret_cast<const clap_event_note*>(event);
#if 0
      Steinberg::Vst::Event oe{};
      oe.type = Steinberg::Vst::Event::kNoteOnEvent;
      oe.noteOn.channel = nevt->channel;
      oe.noteOn.pitch = nevt->key;
      oe.noteOn.velocity = nevt->velocity;
      oe.noteOn.length = 0;
      oe.noteOn.tuning = 0.0f;
      oe.noteOn.noteId = nevt->note_id;
      oe.busIndex = 0;  // FIXME - multi-out midi still needs work
      oe.sampleOffset = nevt->header.time;

      if (_vstdata && _vstdata->outputEvents) _vstdata->outputEvents->addEvent(oe);
#endif
      (void)nevt;
    }
      return true;
    case CLAP_EVENT_NOTE_OFF:
    {
      auto nevt = reinterpret_cast<const clap_event_note*>(event);
#if 0
      Steinberg::Vst::Event oe{};
      oe.type = Steinberg::Vst::Event::kNoteOffEvent;
      oe.noteOff.channel = nevt->channel;
      oe.noteOff.pitch = nevt->key;
      oe.noteOff.velocity = nevt->velocity;
      oe.noteOn.length = 0;
      oe.noteOff.tuning = 0.0f;
      oe.noteOff.noteId = nevt->note_id;
      oe.busIndex = 0;  // FIXME - multi-out midi still needs work
      oe.sampleOffset = nevt->header.time;

      if (_vstdata && _vstdata->outputEvents) _vstdata->outputEvents->addEvent(oe);
#endif
      (void)nevt;
    }
      return true;
    case CLAP_EVENT_NOTE_END:
    case CLAP_EVENT_NOTE_CHOKE:
      removeFromActiveNotes((const clap_event_note*)(event));
      return true;
      break;
    case CLAP_EVENT_NOTE_EXPRESSION:
      return true;
      break;
    case CLAP_EVENT_PARAM_VALUE:
    {
      auto ev = (clap_event_param_value*)event;
#if 0
      auto param = (Vst3Parameter*)this->parameters->getParameter(ev->param_id & 0x7FFFFFFF);
      if (param)
      {
        auto param_id = param->getInfo().id;

        // if the parameter is marked as being edited in the UI, pass the value
        // to the queue so it can be given to the IComponentHandler
        if (std::find(_gesturedParameters.begin(), _gesturedParameters.end(), param_id) !=
            _gesturedParameters.end())
        {
          _automation->onPerformEdit(ev);
        }

        // it also needs to be communicated to the audio thread,otherwise the parameter jumps back to the original value
        Steinberg::int32 index = 0;
        // addParameterData() does check if there is already a queue and returns it,
        // actually, it should be called getParameterQueue()

        // the vst3 validator from the VST3 SDK does not provide always an object to output parameters, probably other hosts won't to that, too
        // therefore we are cautious.
        if (_vstdata->outputParameterChanges)
        {
          auto list = _vstdata->outputParameterChanges->addParameterData(param_id, index);

          // the implementation of addParameterData() in the SDK always returns a queue, but Cubase 12 (perhaps others, too)
          // sometimes don't return a queue object during the first bunch of process calls. I (df) haven't figured out, why.
          // therefore we have to check if there is an output queue at all
          if (list)
          {
            Steinberg::int32 index2 = 0;
            list->addPoint(ev->header.time, param->asVst3Value(ev->value), index2);
          }
        }
      }
#endif
      (void)ev;
    }

      return true;
      break;
    case CLAP_EVENT_PARAM_MOD:
      return true;
      break;
    case CLAP_EVENT_PARAM_GESTURE_BEGIN:
    {
      auto ev = (clap_event_param_gesture*)event;
#if 0
      auto param = (Vst3Parameter*)this->parameters->getParameter(ev->param_id & 0x7FFFFFFF);
      _gesturedParameters.push_back(param->getInfo().id);
      _automation->onBeginEdit(param->getInfo().id);
#endif
      (void)ev;
    }

      return true;

      break;
    case CLAP_EVENT_PARAM_GESTURE_END:
    {
      auto ev = (clap_event_param_gesture*)event;
      (void)ev;
#if 0
      auto param = (Vst3Parameter*)this->parameters->getParameter(ev->param_id & 0x7FFFFFFF);

      auto n = std::remove(_gesturedParameters.begin(), _gesturedParameters.end(), param->getInfo().id);
      if (n != _gesturedParameters.end())
      {
        _gesturedParameters.erase(n, _gesturedParameters.end());
        _automation->onEndEdit(param->getInfo().id);
      }
#endif
    }
      return true;
      break;

    case CLAP_EVENT_MIDI:
    case CLAP_EVENT_MIDI_SYSEX:
    case CLAP_EVENT_MIDI2:
      return true;
      break;
    default:
      break;
  }
  return false;
}

void ProcessAdapter::addToActiveNotes(const clap_event_note* note)
{
  for (auto& i : _activeNotes)
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

void ProcessAdapter::removeFromActiveNotes(const clap_event_note* note)
{
  for (auto& i : _activeNotes)
  {
    if (i.used && i.port_index == note->port_index && i.channel == note->channel &&
        i.note_id == note->note_id)
    {
      i.used = false;
    }
  }
}

void ProcessAdapter::addMIDIEvent(UInt32 inStatus, UInt32 inData1, UInt32 inData2,
                                  UInt32 inOffsetSampleFrame)
{
  const UInt32 strippedStatus = inStatus & 0xf0U;  // NOLINT
  const UInt32 channel = inStatus & 0x0fU;         // NOLINT

  auto deltaFrames = inOffsetSampleFrame & kMusicDeviceSampleFrameMask_SampleOffset;

  bool live = (inOffsetSampleFrame & kMusicDeviceSampleFrameMask_IsScheduled) != 0;

  if (strippedStatus == 0x90)
  {
    clap_multi_event n;
    n.header.time = deltaFrames;
    n.header.type = CLAP_EVENT_NOTE_ON;
    n.header.flags = 0 + (live ? CLAP_EVENT_IS_LIVE : 0);
    n.header.size = sizeof(clap_event_note_t);
    n.header.space_id = 0;
    n.note.port_index = 0;
    n.note.note_id = -1;
    n.note.key = (inData1 & 0x7F);
    n.note.velocity = (inData2 & 0x7F);
    n.note.channel = channel;
    this->_eventindices.emplace_back((this->_events.size()));
    this->_events.emplace_back(n);
  }
  if (strippedStatus == 0x80)
  {
    clap_multi_event n;
    n.header.time = deltaFrames;
    n.header.type = CLAP_EVENT_NOTE_OFF;
    n.header.flags = 0 + (live ? CLAP_EVENT_IS_LIVE : 0);
    n.header.size = sizeof(clap_event_note_t);
    n.header.space_id = 0;
    n.note.port_index = 0;
    n.note.note_id = -1;
    n.note.key = (inData1 & 0x7F);
    n.note.velocity = (inData2 & 0x7F);
    n.note.channel = channel;
    this->_eventindices.emplace_back((this->_events.size()));
    this->_events.emplace_back(n);
  }
}
}  // namespace Clap::AUv2
