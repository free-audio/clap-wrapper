#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"

#include "process.h"

#include <algorithm>
#include <cmath>
#include <cassert>

namespace Clap::AUv3
{

inline clap_beattime doubleToBeatTime(double t)
{
  return std::round(t * CLAP_BEATTIME_FACTOR);
}

inline clap_sectime doubleToSecTime(double t)
{
  return std::round(t * CLAP_SECTIME_FACTOR);
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
  delete[] _silent_input;
  _silent_input = nullptr;
  delete[] _silent_output;
  _silent_output = nullptr;

  if (_inputBufferList)
  {
    free(_inputBufferList);
    _inputBufferList = nullptr;
  }
}

void ProcessAdapter::setupProcessing(uint32_t numInputBusses, const uint32_t *inputChannelCounts,
                                     uint32_t numOutputBusses, const uint32_t *outputChannelCounts,
                                     const clap_plugin_t *plugin,
                                     const clap_plugin_params_t *ext_params,
                                     Clap::IAutomation *automation, uint32_t numMaxSamples,
                                     uint32_t preferredMIDIDialect)
{
  _plugin = plugin;
  _ext_params = ext_params;
  _automation = automation;
  _preferred_midi_dialect = preferredMIDIDialect;

  // Setup silent buffers
  if (numMaxSamples > 0)
  {
    delete[] _silent_input;
    _silent_input = new float[numMaxSamples];
    memset(_silent_input, 0, numMaxSamples * sizeof(float));

    delete[] _silent_output;
    _silent_output = new float[numMaxSamples];
    memset(_silent_output, 0, numMaxSamples * sizeof(float));
  }

  // Setup input ports
  _numInputs = numInputBusses;
  delete[] _input_ports;
  _input_ports = nullptr;

  if (_numInputs > 0)
  {
    _input_ports = new clap_audio_buffer_t[_numInputs];
    for (uint32_t i = 0; i < _numInputs; ++i)
    {
      auto &bus = _input_ports[i];
      bus.channel_count = inputChannelCounts[i];
      bus.constant_mask = 0;
      bus.latency = 0;
      bus.data64 = nullptr;
      bus.data32 = new float *[bus.channel_count];
      for (uint32_t j = 0; j < bus.channel_count; ++j)
      {
        bus.data32[j] = _silent_input;
      }
    }
  }

  // Setup output ports
  _numOutputs = numOutputBusses;
  delete[] _output_ports;
  _output_ports = nullptr;

  if (_numOutputs > 0)
  {
    _output_ports = new clap_audio_buffer_t[_numOutputs];
    for (uint32_t i = 0; i < _numOutputs; ++i)
    {
      auto &bus = _output_ports[i];
      bus.channel_count = outputChannelCounts[i];
      bus.constant_mask = 0;
      bus.latency = 0;
      bus.data64 = nullptr;
      bus.data32 = new float *[bus.channel_count];
      for (uint32_t j = 0; j < bus.channel_count; ++j)
      {
        bus.data32[j] = _silent_output;
      }
    }
  }

  // Allocate input buffer list for pulling input
  if (_numInputs > 0)
  {
    uint32_t maxCh = 0;
    for (uint32_t i = 0; i < _numInputs; ++i)
    {
      if (inputChannelCounts[i] > maxCh) maxCh = inputChannelCounts[i];
    }
    if (_inputBufferList) free(_inputBufferList);
    size_t ablSize = sizeof(AudioBufferList) + (maxCh > 1 ? (maxCh - 1) * sizeof(AudioBuffer) : 0);
    _inputBufferList = (AudioBufferList *)calloc(1, ablSize);
    _inputBufferListChannels = maxCh;
  }

  // Wire up CLAP process data
  _processData.audio_inputs = _input_ports;
  _processData.audio_inputs_count = _numInputs;
  _processData.audio_outputs = _output_ports;
  _processData.audio_outputs_count = _numOutputs;

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
  _eventindices.reserve(8192);
}

void ProcessAdapter::setTransportStateBlock(AUHostTransportStateBlock __nullable block)
{
  _transportStateBlock = block;
}

void ProcessAdapter::sortEventIndices()
{
  std::sort(_eventindices.begin(), _eventindices.end(),
            [&](size_t const &a, size_t const &b)
            {
              auto t1 = _events[a].header.time;
              auto t2 = _events[b].header.time;
              return (t1 == t2) ? (a < b) : (t1 < t2);
            });
}

void ProcessAdapter::translateAUv3Events(const AURenderEvent *head)
{
  for (const AURenderEvent *event = head; event != nullptr; event = event->head.next)
  {
    clap_multi_event_t n;
    memset(&n, 0, sizeof(n));

    switch (event->head.eventType)
    {
      case AURenderEventParameter:
      case AURenderEventParameterRamp:
      {
        auto &pe = event->parameter;
        n.header.size = sizeof(clap_event_param_value_t);
        n.header.type = CLAP_EVENT_PARAM_VALUE;
        n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        n.header.time = (uint32_t)pe.eventSampleTime;
        n.header.flags = 0;

        n.param.param_id = (clap_id)pe.parameterAddress;
        n.param.value = (double)pe.value;
        n.param.port_index = -1;
        n.param.key = -1;
        n.param.channel = -1;
        n.param.note_id = -1;
        n.param.cookie = nullptr;

        _eventindices.emplace_back(_events.size());
        _events.emplace_back(n);
        break;
      }

      case AURenderEventMIDI:
      {
        auto &me = event->MIDI;
        uint8_t status = me.data[0];
        uint8_t strippedStatus = (status >> 4) & 0x0F;
        uint8_t channel = status & 0x0F;

        n.header.time = (uint32_t)me.eventSampleTime;
        n.header.flags = 0;
        n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;

        if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
        {
          if (strippedStatus == 0x09 && me.data[2] > 0)  // Note On
          {
            n.header.type = CLAP_EVENT_NOTE_ON;
            n.header.size = sizeof(clap_event_note_t);
            n.note.port_index = 0;
            n.note.note_id = -1;
            n.note.key = me.data[1] & 0x7F;
            n.note.velocity = (float)(me.data[2] & 0x7F) / 127.0f;
            n.note.channel = channel;

            _eventindices.emplace_back(_events.size());
            _events.emplace_back(n);
            break;
          }
          else if (strippedStatus == 0x08 || (strippedStatus == 0x09 && me.data[2] == 0))  // Note Off
          {
            n.header.type = CLAP_EVENT_NOTE_OFF;
            n.header.size = sizeof(clap_event_note_t);
            n.note.port_index = 0;
            n.note.note_id = -1;
            n.note.key = me.data[1] & 0x7F;
            n.note.velocity = (strippedStatus == 0x08) ? (float)(me.data[2] & 0x7F) / 127.0f : 0.0f;
            n.note.channel = channel;

            _eventindices.emplace_back(_events.size());
            _events.emplace_back(n);
            break;
          }
        }

        // Fall through for non-note MIDI or MIDI dialect preference
        n.header.type = CLAP_EVENT_MIDI;
        n.header.size = sizeof(clap_event_midi_t);
        n.midi.port_index = 0;
        n.midi.data[0] = me.data[0];
        n.midi.data[1] = me.data[1];
        n.midi.data[2] = me.data[2];

        _eventindices.emplace_back(_events.size());
        _events.emplace_back(n);
        break;
      }

      case AURenderEventMIDISysEx:
      {
        // SysEx uses the same AUMIDIEvent struct with extended data
        auto &se = event->MIDI;
        n.header.type = CLAP_EVENT_MIDI_SYSEX;
        n.header.size = sizeof(clap_event_midi_sysex_t);
        n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        n.header.time = (uint32_t)se.eventSampleTime;
        n.header.flags = 0;
        n.sysex.port_index = 0;
        n.sysex.buffer = se.data;
        n.sysex.size = se.length;

        _eventindices.emplace_back(_events.size());
        _events.emplace_back(n);
        break;
      }

      default:
        // AURenderEventMIDIEventList (MIDI 2.0) - not yet supported
        break;
    }
  }
}

AUAudioUnitStatus ProcessAdapter::process(AudioUnitRenderActionFlags *actionFlags,
                                          const AudioTimeStamp *timestamp,
                                          AVAudioFrameCount frameCount,
                                          NSInteger outputBusNumber,
                                          AudioBufferList *outputData,
                                          const AURenderEvent *realtimeEventListHead,
                                          AURenderPullInputBlock __unsafe_unretained pullInputBlock)
{
  // Clear events from previous cycle
  _events.clear();
  _eventindices.clear();

  // Translate AUv3 events to CLAP events
  if (realtimeEventListHead)
  {
    translateAUv3Events(realtimeEventListHead);
  }

  // Sort events by timestamp
  sortEventIndices();

  _processData.frames_count = frameCount;

  // Setup transport
  _transport.flags = 0;
  if (_transportStateBlock)
  {
    AUHostTransportStateFlags transportFlags = 0;
    double currentSamplePosition = 0;
    double cycleStartBeatPosition = 0;
    double cycleEndBeatPosition = 0;

    // Query transport state
    if (_transportStateBlock(&transportFlags, &currentSamplePosition, &cycleStartBeatPosition,
                             &cycleEndBeatPosition))
    {
      if (transportFlags & AUHostTransportStateMoving)
      {
        _transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
      }
      if (transportFlags & AUHostTransportStateRecording)
      {
        _transport.flags |= CLAP_TRANSPORT_IS_RECORDING;
      }
      if (transportFlags & AUHostTransportStateCycling)
      {
        _transport.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
        _transport.loop_start_beats = doubleToBeatTime(cycleStartBeatPosition);
        _transport.loop_end_beats = doubleToBeatTime(cycleEndBeatPosition);
      }
    }

    // Note: AUv3 provides tempo via the musicalContextBlock property
    // which can be queried separately if needed in the future.
  }

  // Pull input audio
  if (_numInputs > 0 && pullInputBlock)
  {
    for (uint32_t bus = 0; bus < _numInputs; ++bus)
    {
      uint32_t numCh = _input_ports[bus].channel_count;

      // Setup the input buffer list for this bus
      _inputBufferList->mNumberBuffers = numCh;
      for (uint32_t ch = 0; ch < numCh; ++ch)
      {
        _inputBufferList->mBuffers[ch].mNumberChannels = 1;
        _inputBufferList->mBuffers[ch].mDataByteSize = frameCount * sizeof(float);
        _inputBufferList->mBuffers[ch].mData = nullptr;  // let the host provide the buffer
      }

      AudioUnitRenderActionFlags pullFlags = 0;
      AUAudioUnitStatus status = pullInputBlock(&pullFlags, timestamp, frameCount, bus, _inputBufferList);
      if (status == noErr)
      {
        for (uint32_t ch = 0; ch < numCh && ch < _inputBufferList->mNumberBuffers; ++ch)
        {
          _input_ports[bus].data32[ch] = (float *)_inputBufferList->mBuffers[ch].mData;
        }
      }
      else
      {
        // Fill with silence on pull failure
        for (uint32_t ch = 0; ch < numCh; ++ch)
        {
          _input_ports[bus].data32[ch] = _silent_input;
        }
      }
    }
  }

  // Wire output buffers
  if (outputData && outputBusNumber < _numOutputs)
  {
    uint32_t outBus = (uint32_t)outputBusNumber;
    uint32_t numCh = std::min((uint32_t)outputData->mNumberBuffers, _output_ports[outBus].channel_count);
    for (uint32_t ch = 0; ch < numCh; ++ch)
    {
      _output_ports[outBus].data32[ch] = (float *)outputData->mBuffers[ch].mData;
    }
  }

  // Process!
  _plugin->process(_plugin, &_processData);

  // Process output events
  for (auto &evt : _outevents)
  {
    switch (evt.header.type)
    {
      case CLAP_EVENT_PARAM_VALUE:
        if (_automation)
        {
          _automation->onPerformEdit(&evt.param);
        }
        break;
      case CLAP_EVENT_PARAM_GESTURE_BEGIN:
      {
        auto *ge = (clap_event_param_gesture *)&evt;
        if (_automation) _automation->onBeginEdit(ge->param_id);
        break;
      }
      case CLAP_EVENT_PARAM_GESTURE_END:
      {
        auto *ge = (clap_event_param_gesture *)&evt;
        if (_automation) _automation->onEndEdit(ge->param_id);
        break;
      }
      case CLAP_EVENT_NOTE_ON:
      case CLAP_EVENT_NOTE_OFF:
      case CLAP_EVENT_MIDI:
      {
        if (midiOutputEventBlock)
        {
          if (evt.header.type == CLAP_EVENT_MIDI)
          {
            midiOutputEventBlock(timestamp->mSampleTime + evt.header.time, 0, 3, evt.midi.data);
          }
          else if (evt.header.type == CLAP_EVENT_NOTE_ON)
          {
            uint8_t data[3] = {(uint8_t)(0x90 | (evt.note.channel & 0x0F)),
                               (uint8_t)(evt.note.key & 0x7F),
                               (uint8_t)(uint8_t)(evt.note.velocity * 127.0f)};
            midiOutputEventBlock(timestamp->mSampleTime + evt.header.time, 0, 3, data);
          }
          else if (evt.header.type == CLAP_EVENT_NOTE_OFF)
          {
            uint8_t data[3] = {(uint8_t)(0x80 | (evt.note.channel & 0x0F)),
                               (uint8_t)(evt.note.key & 0x7F),
                               (uint8_t)(uint8_t)(evt.note.velocity * 127.0f)};
            midiOutputEventBlock(timestamp->mSampleTime + evt.header.time, 0, 3, data);
          }
        }
        break;
      }
      default:
        break;
    }
  }
  _outevents.clear();

  return noErr;
}

void ProcessAdapter::addParameterEvent(clap_id paramId, double value, uint32_t sampleOffset)
{
  clap_multi_event_t n;
  memset(&n, 0, sizeof(n));
  n.header.size = sizeof(clap_event_param_value_t);
  n.header.type = CLAP_EVENT_PARAM_VALUE;
  n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
  n.header.time = sampleOffset;
  n.header.flags = 0;

  n.param.value = value;
  n.param.param_id = paramId;
  n.param.cookie = nullptr;
  n.param.port_index = -1;
  n.param.key = -1;
  n.param.channel = -1;
  n.param.note_id = -1;

  _eventindices.emplace_back(_events.size());
  _events.emplace_back(n);
}

// --- CLAP event callbacks ---

uint32_t ProcessAdapter::input_events_size(const struct clap_input_events *list)
{
  auto self = static_cast<ProcessAdapter *>(list->ctx);
  return (uint32_t)self->_events.size();
}

const clap_event_header_t *ProcessAdapter::input_events_get(const struct clap_input_events *list,
                                                            uint32_t index)
{
  auto self = static_cast<ProcessAdapter *>(list->ctx);
  if (index < self->_events.size())
  {
    auto realindex = self->_eventindices[index];
    return &(self->_events[realindex].header);
  }
  return nullptr;
}

bool ProcessAdapter::output_events_try_push(const struct clap_output_events *list,
                                            const clap_event_header_t *event)
{
  auto self = static_cast<ProcessAdapter *>(list->ctx);
  return self->enqueueOutputEvent(event);
}

bool ProcessAdapter::enqueueOutputEvent(const clap_event_header_t *event)
{
  if (event->size <= sizeof(clap_multi_event_t))
  {
    clap_multi_event_t e;
    memcpy(&e, event, event->size);
    _outevents.emplace_back(e);
    return true;
  }
  return false;
}

}  // namespace Clap::AUv3

#pragma clang diagnostic pop
