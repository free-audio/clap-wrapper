#include "process.h"
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstcomponent.h>

#include "parameter.h"
#include <algorithm>

#include <cmath>
#include "../clap/automation.h"


namespace Clap
{
  using namespace Steinberg;

  void ProcessAdapter::setupProcessing(const clap_plugin_t* plugin, const clap_plugin_params_t* ext_params,
    Vst::BusList& audioinputs, Vst::BusList& audiooutputs,
    uint32_t numSamples, size_t numEventInputs, size_t numEventOutputs, 
    Steinberg::Vst::ParameterContainer& params, Steinberg::Vst::IComponentHandler* componenthandler,
    IAutomation* automation, bool enablePolyPressure)
  {
    _plugin = plugin;
    _ext_params = ext_params;
    _audioinputs = &audioinputs;
    _audiooutputs = &audiooutputs;

    parameters = &params;
    _componentHandler = componenthandler;
    _automation = automation;    

    if (numSamples > 0)
    {
      delete[] _silent_input;
      _silent_input = new float[numSamples];

      delete[] _silent_output;
      _silent_output = new float[numSamples];
    }

    auto numInputs = _audioinputs->size();
    auto numOutputs = _audiooutputs->size();

    _processData.audio_inputs_count = numInputs;
    delete[] _input_ports;
    _input_ports = nullptr;

    if ( numInputs > 0)
    {
      _input_ports = new clap_audio_buffer_t[numInputs];
      for (int i = 0; i < numInputs; ++i)
      {
        clap_audio_buffer_t& bus = _input_ports[i];
        Vst::BusInfo info;
        if (_audioinputs->at(i)->getInfo(info))
        {
          bus.channel_count = info.channelCount;
          bus.constant_mask = 0;
          bus.latency = 0;
          bus.data64 = 0;
          bus.data32 = 0;
        }
      }
      _processData.audio_inputs = _input_ports;
    }
    else
    {
      _processData.audio_inputs = nullptr;
    }

    _processData.audio_outputs_count = numOutputs;
    delete[] _output_ports;
    _output_ports = nullptr;


    if (numOutputs > 0)
    {
      _output_ports = new clap_audio_buffer_t[numOutputs];
      for (int i = 0; i < numOutputs; ++i)
      {
        clap_audio_buffer_t& bus = _output_ports[i];
        Vst::BusInfo info;
        if (_audiooutputs->at(i)->getInfo(info))
        {
          bus.channel_count = info.channelCount;
          bus.constant_mask = 0;
          bus.latency = 0;
          bus.data64 = 0;
          bus.data32 = 0;
        }
      }
      _processData.audio_outputs = _output_ports;
    }
    else
    {
      _processData.audio_outputs = nullptr;
    }
    
    _processData.in_events = &_in_events;
    _processData.out_events = &_out_events;

    _processData.transport = &_transport;

    _in_events.ctx = this;
    _in_events.size = input_events_size;
    _in_events.get = input_events_get;

    _out_events.ctx = this;
    _out_events.try_push = output_events_try_push;

    _events.clear();
    _events.reserve(256);
    _eventindices.clear();
    _eventindices.reserve(_events.capacity());

    _out_events.ctx = this;

    _gesturedParameters.reserve(32);

    _activeNotes.reserve(64);

    _supportsPolyPressure = enablePolyPressure;

  }  

  void ProcessAdapter::activateAudioBus(Steinberg::Vst::BusDirection dir, int32 index, TBool state)
  {
    /*
    if (dir == Vst::kInput)
    {
      auto& map = _bogus_buffers->_in_channelmap[index];
      if (state)
        _bogus_buffers->_active_mask_in |= map._bitmap;
      else
        _bogus_buffers->_active_mask_in &= ~map._bitmap;
    }
    if (dir == Vst::kOutput)
    {
      auto& map = _bogus_buffers->_out_channelmap[index];
      if (state)
        _bogus_buffers->_active_mask_out |= map._bitmap;
      else
        _bogus_buffers->_active_mask_out &= ~map._bitmap;
    }
    */
  }

  inline clap_beattime doubleToBeatTime(double t)
  {
    return std::round(t * CLAP_BEATTIME_FACTOR);
  }

  inline clap_sectime doubleToSecTime(double t)
  {
    return round(t * CLAP_SECTIME_FACTOR);
  }


  void ProcessAdapter::flush()
  {
    // minimal processing if _ext_params is existent
    if (_ext_params)
    {
      _events.clear();
      _eventindices.clear();
      
      // sortEventIndices(); call only if there would be any input event
      _ext_params->flush(_plugin, _processData.in_events, _processData.out_events);
    }
  }

  // this converts the ProcessContext data from VST to CLAP
  void ProcessAdapter::process(Steinberg::Vst::ProcessData& data)
  {
    // remember the ProcessData pointer during process
    _vstdata = &data;

    /// convert timing
    _transport.header = {
      sizeof(_transport),
      0,
      CLAP_CORE_EVENT_SPACE_ID,
      CLAP_EVENT_TRANSPORT,
      0
    };

    _transport.flags = 0;
    if (_vstdata->processContext)
    {
      // converting the flags
      _transport.flags |= 0
        // kPlaying = 1 << 1,		///< currently playing
        | ((_vstdata->processContext->state & Vst::ProcessContext::kPlaying) ? CLAP_TRANSPORT_IS_PLAYING : 0)
        // kRecording = 1 << 3,		///< currently recording
        | ((_vstdata->processContext->state & Vst::ProcessContext::kRecording) ? CLAP_TRANSPORT_IS_RECORDING : 0)
        // kCycleActive = 1 << 2,		///< cycle is active
        | ((_vstdata->processContext->state & Vst::ProcessContext::kCycleActive) ? CLAP_TRANSPORT_IS_LOOP_ACTIVE : 0)
        // kTempoValid = 1 << 10,	///< tempo contains valid information
        | ((_vstdata->processContext->state & Vst::ProcessContext::kTempoValid) ? CLAP_TRANSPORT_HAS_TEMPO : 0)
        | ((_vstdata->processContext->state & Vst::ProcessContext::kBarPositionValid) ? CLAP_TRANSPORT_HAS_BEATS_TIMELINE : 0)
        | ((_vstdata->processContext->state & Vst::ProcessContext::kTimeSigValid) ? CLAP_TRANSPORT_HAS_TIME_SIGNATURE : 0)

        // the rest of the flags has no meaning to CLAP
        // kSystemTimeValid = 1 << 8,		///< systemTime contains valid information
        // kContTimeValid = 1 << 17,	///< continousTimeSamples contains valid information
        // 
        // kProjectTimeMusicValid = 1 << 9,///< projectTimeMusic contains valid information
        // kBarPositionValid = 1 << 11,	///< barPositionMusic contains valid information
        // kCycleValid = 1 << 12,	///< cycleStartMusic and barPositionMusic contain valid information
        // 
        // kClockValid = 1 << 15		///< samplesToNextClock valid
        // kTimeSigValid = 1 << 13,	///< timeSigNumerator and timeSigDenominator contain valid information
        // kChordValid = 1 << 18,	///< chord contains valid information
        // 
        // kSmpteValid = 1 << 14,	///< smpteOffset and frameRate contain valid information

        ;

      _transport.song_pos_beats = doubleToBeatTime(_vstdata->processContext->projectTimeMusic);
      _transport.song_pos_seconds = 0;

      _transport.tempo = _vstdata->processContext->tempo;
      _transport.tempo_inc = 0;

      _transport.loop_start_beats = doubleToBeatTime(_vstdata->processContext->cycleStartMusic);
      _transport.loop_end_beats = doubleToBeatTime(_vstdata->processContext->cycleEndMusic);
      _transport.loop_start_seconds = 0;
      _transport.loop_end_seconds = 0;

      _transport.bar_start = 0;
      _transport.bar_number = 0;

      if ((_vstdata->processContext->state & Vst::ProcessContext::kTimeSigValid))
      {
        _transport.tsig_num = _vstdata->processContext->timeSigNumerator;
        _transport.tsig_denom = _vstdata->processContext->timeSigDenominator;
      }
      else
      {
        _transport.tsig_num = 4;
        _transport.tsig_denom = 4;
      }

      _transport.bar_number = _vstdata->processContext->barPositionMusic;
      _processData.steady_time = _vstdata->processContext->projectTimeSamples;
    }

    // setting up transport
    _processData.frames_count = _vstdata->numSamples;

    // always clear
    _events.clear();
    _eventindices.clear();

    processInputEvents(_vstdata->inputEvents);

    if (_vstdata->inputParameterChanges)
    {
      auto numPevent = _vstdata->inputParameterChanges->getParameterCount();
      for (decltype(numPevent) i = 0; i < numPevent; ++i)
      {
        auto k = _vstdata->inputParameterChanges->getParameterData(i);

        // get the Vst3Parameter
        auto paramid = k->getParameterId();

        auto param = (Vst3Parameter*)parameters->getParameter(paramid);
        if (param)
        {
          if (param->isMidi)
          {

            auto nums = k->getPointCount();

            Vst::ParamValue value;
            int32 offset;
            if (k->getPoint(nums - 1, offset, value) == kResultOk)
            {
              // create MIDI event
              clap_multi_event_t n;
              n.param.header.type = CLAP_EVENT_MIDI;
              n.param.header.flags = 0;
              n.param.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
              n.param.header.time = offset;
              n.param.header.size = sizeof(clap_event_midi_t);
              n.midi.port_index = 0;

              switch (param->controller)
              {
              case Vst::ControllerNumbers::kAfterTouch:
                n.midi.data[0] = 0xD0 | param->channel;
                n.midi.data[1] = param->asClapValue(value);
                n.midi.data[2] = 0;
                break;
              case Vst::ControllerNumbers::kPitchBend:
              {
                auto val = (uint16_t)param->asClapValue(value);
                n.midi.data[0] = 0xE0 | param->channel; // $Ec
                n.midi.data[1] = (val & 0x7F);          // LSB
                n.midi.data[2] = (val >> 7) & 0x7F;     // MSB
              }
              break;
              default:
                n.midi.data[0] = 0xB0 | param->channel;
                n.midi.data[1] = param->controller;
                n.midi.data[2] = param->asClapValue(value);
                break;
              }

              _eventindices.push_back(_events.size());
              _events.push_back(n);
            }
          }
          else
          {
            auto nums = k->getPointCount();

            Vst::ParamValue value;
            int32 offset;
            if (k->getPoint(nums - 1, offset, value) == kResultOk)
            {
              clap_multi_event_t n;
              n.param.header.type = CLAP_EVENT_PARAM_VALUE;
              n.param.header.flags = 0;
              n.param.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
              n.param.header.time = offset;
              n.param.header.size = sizeof(clap_event_param_value);
              n.param.param_id = param->id;
              n.param.cookie = param->cookie;

              // nothing note specific
              n.param.note_id = -1;   // always global
              n.param.port_index = -1;
              n.param.channel = -1;
              n.param.key = -1;

              n.param.value = param->asClapValue(value);
              _eventindices.push_back(_events.size());
              _events.push_back(n);
            }
          }
        }

      }
    }

    sortEventIndices();

    bool doProcess = true;

    if (_vstdata->numSamples > 0 )
    {
      // setting the buffers
      auto inbusses = _audioinputs->size();
      for (int i = 0; i < inbusses; ++i)
      {
        if (_vstdata->inputs[i].numChannels > 0)
          _input_ports[i].data32 = _vstdata->inputs[i].channelBuffers32;
        else
          doProcess = false;
      }

      auto outbusses = _audiooutputs->size();
      for (int i = 0; i < outbusses; ++i)
      {
        if (_vstdata->outputs[i].numChannels > 0)
          _output_ports[i].data32 = _vstdata->outputs[i].channelBuffers32;
        else
          doProcess = false;
      }
      if (doProcess)
        _plugin->process(_plugin, &_processData);
      else
      {
        if (_ext_params)
        {
          _ext_params->flush(_plugin, _processData.in_events, _processData.out_events);
        }
      }
    }
    else
    {
      if (_ext_params)
      {
        _ext_params->flush(_plugin, _processData.in_events, _processData.out_events);
      }
      else
      {
        // something was now very very wrong here..
      }
    }

    processOutputParams(data);

    _vstdata = nullptr;
  }

  void ProcessAdapter::processOutputParams(Steinberg::Vst::ProcessData& data)
  {

  }

  uint32_t ProcessAdapter::input_events_size(const struct clap_input_events* list)
  {
    auto self = static_cast<ProcessAdapter*>(list->ctx);
    return self->_events.size();
    // return self->_vstdata->inputEvents->getEventCount();
  }

  // returns the pointer to an event in the list. The index accessed is not the position in the event list itself
  // since all events indices were sorted by timestamp
  const clap_event_header_t* ProcessAdapter::input_events_get(const struct clap_input_events* list, uint32_t index)
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

  bool ProcessAdapter::output_events_try_push(const struct clap_output_events* list, const clap_event_header_t* event)
  {
    auto self = static_cast<ProcessAdapter*>(list->ctx);
    // mainly used for CLAP_EVENT_NOTE_CHOKE and CLAP_EVENT_NOTE_END
    // but also for parameter changes
    return self->enqueueOutputEvent(event);
  }

  void ProcessAdapter::sortEventIndices()
  {
    // just sorting the index
    std::sort(_eventindices.begin(), _eventindices.end(), [&](size_t const& a, size_t const& b)
      {
        return _events[a].header.time < _events[b].header.time;
      }
    );
  }

  void ProcessAdapter::processInputEvents(Steinberg::Vst::IEventList* eventlist)
  {
    if (eventlist)
    {
      Vst::Event vstevent;
      auto numev = eventlist->getEventCount();
      for (decltype(numev) i = 0; i < numev; ++i)
      {
        if (eventlist->getEvent(i, vstevent) == kResultOk)
        {
          if (vstevent.type == Vst::Event::kNoteOnEvent)
          {
            clap_multi_event_t n;
            n.note.header.type = CLAP_EVENT_NOTE_ON;
            n.note.header.flags = (vstevent.flags & Vst::Event::kIsLive) ? CLAP_EVENT_IS_LIVE : 0;
            n.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            n.note.header.time = vstevent.sampleOffset;
            n.note.header.size = sizeof(clap_event_note);
            n.note.channel = vstevent.noteOn.channel;
            n.note.note_id = vstevent.noteOn.noteId;
            n.note.port_index = 0;
            n.note.velocity = vstevent.noteOn.velocity;
            n.note.key = vstevent.noteOn.pitch;
            _eventindices.push_back(_events.size());
            _events.push_back(n);
            addToActiveNotes(&n.note);
          }
          if (vstevent.type == Vst::Event::kNoteOffEvent)
          {
            clap_multi_event_t n;
            n.note.header.type = CLAP_EVENT_NOTE_OFF;
            n.note.header.flags = (vstevent.flags & Vst::Event::kIsLive) ? CLAP_EVENT_IS_LIVE : 0;
            n.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            n.note.header.time = vstevent.sampleOffset;
            n.note.header.size = sizeof(clap_event_note);
            n.note.channel = vstevent.noteOff.channel;
            n.note.note_id = vstevent.noteOff.noteId;
            n.note.port_index = 0;
            n.note.velocity = vstevent.noteOff.velocity;
            n.note.key = vstevent.noteOff.pitch;
            _eventindices.push_back(_events.size());
            _events.push_back(n);
          }
          if (vstevent.type == Vst::Event::kDataEvent)
          {
            clap_multi_event_t n;
            if (vstevent.data.type == Vst::DataEvent::DataTypes::kMidiSysEx)
            {
              n.sysex.buffer = vstevent.data.bytes;
              n.sysex.size = vstevent.data.size;
              n.sysex.port_index = 0;
              n.sysex.header.type = CLAP_EVENT_MIDI_SYSEX;
              n.sysex.header.flags = vstevent.flags & Vst::Event::kIsLive ? CLAP_EVENT_IS_LIVE : 0;
              n.sysex.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
              n.sysex.header.time = vstevent.sampleOffset;
              n.sysex.header.size = sizeof(n.sysex);
              _eventindices.push_back(_events.size());
              _events.push_back(n);
            }
            else
            {
              // there are no other event types yet
            }
          }
          if (_supportsPolyPressure && vstevent.type == Vst::Event::kPolyPressureEvent)
          {
            clap_multi_event_t n;
            auto& f = vstevent.noteExpressionValue;
            n.noteexpression.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            n.noteexpression.header.flags = (vstevent.flags & Vst::Event::kIsLive) ? CLAP_EVENT_IS_LIVE : 0;
            n.noteexpression.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            n.noteexpression.header.time = vstevent.sampleOffset;
            n.noteexpression.header.size = sizeof(clap_event_note_expression);
            n.noteexpression.note_id = vstevent.polyPressure.noteId;
            for (auto& i : _activeNotes)
            {
              if (i.used && i.note_id == vstevent.polyPressure.noteId)
              {
                n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
                n.noteexpression.port_index = i.port_index;
                n.noteexpression.key = i.key;   // should be the same as vstevent.polyPressure.pitch
                n.noteexpression.channel = i.channel;
                n.noteexpression.value = vstevent.polyPressure.pressure;
              }
            }
            _eventindices.push_back(_events.size());
            _events.push_back(n);
          }
          if (vstevent.type == Vst::Event::kNoteExpressionValueEvent)
          {
            clap_multi_event_t n;
            auto& f = vstevent.noteExpressionValue;
            n.noteexpression.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            n.noteexpression.header.flags = (vstevent.flags & Vst::Event::kIsLive) ? CLAP_EVENT_IS_LIVE : 0;
            n.noteexpression.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            n.noteexpression.header.time = vstevent.sampleOffset;
            n.noteexpression.header.size = sizeof(clap_event_note_expression);
            n.noteexpression.note_id = vstevent.noteExpressionValue.noteId;
            for (auto& i : _activeNotes)
            {
              if (i.used && i.note_id == vstevent.noteExpressionValue.noteId)
              {
                n.noteexpression.port_index = i.port_index;
                n.noteexpression.key = i.key;
                n.noteexpression.channel = i.channel;
                n.noteexpression.value = vstevent.noteExpressionValue.value;
                switch (vstevent.noteExpressionValue.typeId)
                {
                case Vst::NoteExpressionTypeIDs::kVolumeTypeID:
                  n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_VOLUME;
                  break;
                case Vst::NoteExpressionTypeIDs::kPanTypeID:
                  n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_PAN;
                  break;
                case Vst::NoteExpressionTypeIDs::kTuningTypeID:
                  n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
                  break;
                case Vst::NoteExpressionTypeIDs::kVibratoTypeID:
                  n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_VIBRATO;
                  break;
                case Vst::NoteExpressionTypeIDs::kExpressionTypeID:
                  n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_EXPRESSION;
                  break;
                case Vst::NoteExpressionTypeIDs::kBrightnessTypeID:
                  n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_BRIGHTNESS;
                  break;
                default:
                  continue;
                }
                _eventindices.push_back(_events.size());
                _events.push_back(n);
              }
            }
          }
        }
      }
    }
  }

  bool ProcessAdapter::enqueueOutputEvent(const clap_event_header_t* event)
  {
    switch (event->type)
    {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
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
      auto param = (Vst3Parameter*)this->parameters->getParameter(ev->param_id);
      if (param)
      {
        auto param_id = ev->param_id;
        // if the parameter is marked as being edited in the UI, pass the value
        // to the queue so it can be given to the IComponentHandler
        if (std::find(_gesturedParameters.begin(), _gesturedParameters.end(), param_id) != _gesturedParameters.end())
        {
          _automation->onPerformEdit(ev);
        }

        // it also needs to be communicated to the audio thread,otherwise the parameter jumps back to the original value
        Steinberg::int32 index = 0;
        // addParameterData() does check if there is already a queue and returns it,
        // actually, it should be called getParameterQueue()
        auto list = _vstdata->outputParameterChanges->addParameterData(param->id, index);

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

    return true;
    break;
    case CLAP_EVENT_PARAM_MOD:
      return true;
      break;
    case CLAP_EVENT_PARAM_GESTURE_BEGIN:
      {
        auto ev = (clap_event_param_gesture*)event;
        _gesturedParameters.push_back(ev->param_id);
        _automation->onBeginEdit(ev->param_id);
      }
      return true;

      break;
    case CLAP_EVENT_PARAM_GESTURE_END:
      {
        auto ev = (clap_event_param_gesture*)event;
        _automation->onEndEdit(ev->param_id);
        _gesturedParameters.erase(std::remove(_gesturedParameters.begin(), _gesturedParameters.end(), ev->param_id));
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
    _activeNotes.push_back({true,note->note_id, note->port_index, note->channel, note->key});
  }

  void ProcessAdapter::removeFromActiveNotes(const clap_event_note * note)
  {
    for (auto& i : _activeNotes)
    {
      if (i.used
        && i.port_index == note->port_index 
        && i.channel == note->channel 
        && i.note_id == note->note_id)
      {
        i.used = false;
      }
    }
  }


}
