#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"

#include "process.h"

#include <os/log.h>
#include <algorithm>
#include <cmath>
#include <cassert>

// static os_log_t _procLog() {
//  static os_log_t log = os_log_create("org.clap-wrapper.auv3", "process");
//  return log;
// }
#define PROCLOG(...)  // os_log(_procLog(), __VA_ARGS__)
#define PROCERR(...)  // os_log_error(_procLog(), __VA_ARGS__)

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
                                     const clap_plugin_t *plugin, const clap_plugin_params_t *ext_params,
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

  // Allocate output storage for multi-bus rendering, sized to the actual
  // channel count of each bus (prefix offsets index into the flat array).
  _numMaxSamples = numMaxSamples;
  _outputStorageOffset.assign(_numOutputs + 1, 0);
  for (uint32_t i = 0; i < _numOutputs; ++i)
  {
    _outputStorageOffset[i + 1] = _outputStorageOffset[i] + outputChannelCounts[i];
  }
  _outputStorage.resize(_outputStorageOffset[_numOutputs]);
  for (auto &buf : _outputStorage) buf.resize(numMaxSamples, 0.0f);
  _lastProcessedSampleTime = std::numeric_limits<double>::quiet_NaN();

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

  // Reserved up front so enqueueOutputEvent normally never allocates on
  // the render thread. Should a plugin ever push more than 8192 output
  // events in one block, we accept the (rare, one-time) growth rather
  // than drop events — the vector never shrinks, so it amortizes to zero.
  _outevents.clear();
  _outevents.reserve(8192);

  _activeNotes.clear();
  _activeNotes.reserve(32);
  _reorderScratch.clear();
  _reorderScratch.reserve(128);
  _nextNoteId = 0;
}

void ProcessAdapter::setTransportStateBlock(AUHostTransportStateBlock __nullable block)
{
  _transportStateBlock = block;
}

void ProcessAdapter::setMusicalContextBlock(AUHostMusicalContextBlock __nullable block)
{
  _musicalContextBlock = block;
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

void ProcessAdapter::reorderSameSampleOrphanOffs(AVAudioFrameCount frameCount)
{
  // Apple's AU scheduler does not guarantee arrival-order delivery for
  // events tagged AUEventSampleTimeImmediate. A very short keyboard tap
  // (press + release inside one MIDI burst) can arrive at the plugin as
  // [NOTE_OFF, NOTE_ON] at the same sample offset even though the hardware
  // sent them in the opposite order. The hosted plugin then sees the
  // NOTE_OFF first, finds no matching playing voice, drops it, then sees
  // the NOTE_ON, starts a voice — and that voice never gets a release.
  // User-visible symptom: stuck notes on fast taps.
  //
  // Fix: walk the already-sorted event list and shadow-replay the plugin's
  // active-note set. If we see a NOTE_OFF for a key that is NOT currently
  // active AND the immediately-following event is a same-time NOTE_ON for
  // the same (port, channel, key), swap the two so NOTE_ON precedes
  // NOTE_OFF, and (when frameCount allows) bump the NOTE_OFF to offset+1
  // so downstream consumers that don't honour stable tiebreak still see
  // the off strictly after the on.
  //
  // A legitimate retrigger [NOTE_OFF, NOTE_ON] for a key whose voice IS
  // playing hits the "already active" branch and is left untouched.
  if (_eventindices.empty()) return;

  // Reused member scratch — a local vector would malloc/free on the render
  // thread every cycle.
  auto &active = _reorderScratch;
  active.clear();

  auto packKey = [](int16_t port, int16_t channel, int16_t key) -> uint32_t
  {
    return ((uint32_t)(uint16_t)port << 16) | ((uint32_t)(uint16_t)channel << 8) |
           ((uint32_t)(uint16_t)(key & 0x7f));
  };

  for (size_t i = 0; i < _eventindices.size(); ++i)
  {
    auto &e = _events[_eventindices[i]];
    if (e.header.type == CLAP_EVENT_NOTE_ON)
    {
      active.emplace_back(packKey(e.note.port_index, e.note.channel, e.note.key));
      continue;
    }
    if (e.header.type != CLAP_EVENT_NOTE_OFF) continue;

    uint32_t k = packKey(e.note.port_index, e.note.channel, e.note.key);
    auto it = std::find(active.begin(), active.end(), k);
    if (it != active.end())
    {
      // Legitimate off (or retrigger pair) — leave order alone.
      active.erase(it);
      continue;
    }

    // Orphan off. Only reorder if the very next event is a same-time
    // NOTE_ON for the same key triple; that's the reordered press-release
    // pattern. Any other orphan off (spurious off, truly unmatched) is
    // left alone — the plugin will just drop it as before.
    if (i + 1 >= _eventindices.size()) continue;
    auto &next = _events[_eventindices[i + 1]];
    if (next.header.type != CLAP_EVENT_NOTE_ON) continue;
    if (next.header.time != e.header.time) continue;
    uint32_t nk = packKey(next.note.port_index, next.note.channel, next.note.key);
    if (nk != k) continue;

    std::swap(_eventindices[i], _eventindices[i + 1]);
    if (frameCount > 1 && e.header.time + 1 < frameCount)
    {
      e.header.time = e.header.time + 1;
      // The bump happens after sorting, so restore monotonicity: bubble the
      // NOTE_OFF (now at i+1) past any remaining events still at the old
      // time, otherwise the plugin would see an event at t+1 followed by
      // events at t — a violation of CLAP's sorted-input contract.
      for (size_t j = i + 1;
           j + 1 < _eventindices.size() && _events[_eventindices[j + 1]].header.time < e.header.time;
           ++j)
      {
        std::swap(_eventindices[j], _eventindices[j + 1]);
      }
    }

    PROCLOG("reorderSameSampleOrphanOffs: swapped orphan off-then-on "
            "port=%d ch=%d key=%d on.t=%u off.t=%u",
            (int)e.note.port_index, (int)e.note.channel, (int)e.note.key, (unsigned)next.header.time,
            (unsigned)e.header.time);

    // Re-examine position i on the next iteration — it is now the NOTE_ON,
    // which needs to enter `active` via the normal NOTE_ON branch. The
    // following iteration will then see the NOTE_OFF at i+1 and erase it.
    --i;
  }
}

void ProcessAdapter::translateAUv3Events(const AURenderEvent *head, AUEventSampleTime bufferStartTime,
                                         AVAudioFrameCount frameCount)
{
  for (const AURenderEvent *event = head; event != nullptr; event = event->head.next)
  {
    clap_multi_event_t n;
    memset(&n, 0, sizeof(n));

    // Convert AUv3 absolute sample time to CLAP buffer-relative offset.
    // Events may be scheduled at AUEventSampleTimeImmediate (0xffffffff00000000,
    // "now") PLUS an optional buffer offset in the low 32 bits — the documented
    // AU pattern for intra-buffer immediate scheduling. As int64 that whole
    // encoding range is [-2^32, -1], which COLLIDES with legitimate absolute
    // times of hosts whose render timeline is negative (pre-roll/priming).
    // Disambiguate by preferring the absolute interpretation whenever it
    // lands inside this buffer; only then decode as Immediate+offset.
    auto absTime = event->head.eventSampleTime;
    uint32_t sampleOffset = 0;
    bool resolved = false;
    if (absTime != AUEventSampleTimeImmediate)
    {
      int64_t rel = absTime - bufferStartTime;
      if (rel >= 0 && rel < (int64_t)frameCount)
      {
        sampleOffset = (uint32_t)rel;
        resolved = true;
      }
    }
    if (!resolved && (uint64_t)absTime >= (uint64_t)AUEventSampleTimeImmediate)
    {
      uint64_t off = (uint64_t)absTime - (uint64_t)AUEventSampleTimeImmediate;
      // Out-of-range offsets degrade to plain "now" (0) — a genuine
      // Immediate offset is documented to be within the buffer.
      if (off < frameCount) sampleOffset = (uint32_t)off;
    }

    switch (event->head.eventType)
    {
      case AURenderEventParameter:
      case AURenderEventParameterRamp:
      {
        // AUv3 parameter events translate to CLAP_EVENT_PARAM_VALUE only.
        // CLAP also has CLAP_EVENT_PARAM_MOD (per-event parameter modulation,
        // optionally polyphonic via note_id) — there is no AUv3 equivalent
        // and AU hosts never produce mod-shaped events. Plugins that declare
        // CLAP_PARAM_IS_MODULATABLE will only receive ordinary value events
        // here. This is a documented limitation of the AUv3 host model;
        // there is no smart way to bridge it.
        auto &pe = event->parameter;
        PROCLOG("translateEvent: param addr=%llu value=%.4f absTime=%lld offset=%u",
                (unsigned long long)pe.parameterAddress, (float)pe.value, (long long)pe.eventSampleTime,
                sampleOffset);
        n.header.size = sizeof(clap_event_param_value_t);
        n.header.type = CLAP_EVENT_PARAM_VALUE;
        n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        n.header.time = sampleOffset;
        n.header.flags = 0;

        clap_id pid = (clap_id)pe.parameterAddress;

        // Skip unknown parameter IDs — auval sends bogus IDs to test robustness
        auto cookieIt = _cookieCache.find(pid);
        if (cookieIt == _cookieCache.end()) break;

        n.param.param_id = pid;
        n.param.value = (double)pe.value;
        n.param.port_index = -1;
        n.param.key = -1;
        n.param.channel = -1;
        n.param.note_id = -1;
        n.param.cookie = cookieIt->second;

        _eventindices.emplace_back(_events.size());
        _events.emplace_back(n);
        break;
      }

      case AURenderEventMIDI:
      {
        auto &me = event->MIDI;
        PROCLOG("translateEvent: %02x %02x %02x", (int)me.data[0], (int)me.data[1], (int)me.data[2]);
        uint8_t status = me.data[0];
        uint8_t strippedStatus = (status >> 4) & 0x0F;
        uint8_t channel = status & 0x0F;

        n.header.time = sampleOffset;
        n.header.flags = 0;
        n.header.space_id = CLAP_CORE_EVENT_SPACE_ID;

        if (_preferred_midi_dialect == CLAP_NOTE_DIALECT_CLAP)
        {
          if (strippedStatus == 0x09 && me.data[2] > 0)  // Note On
          {
            n.header.type = CLAP_EVENT_NOTE_ON;
            n.header.size = sizeof(clap_event_note_t);
            n.note.port_index = 0;
            n.note.note_id = synthesizeNoteId();
            n.note.key = me.data[1] & 0x7F;
            n.note.velocity = (float)(me.data[2] & 0x7F) / 127.0f;
            n.note.channel = channel;

            _eventindices.emplace_back(_events.size());
            _events.emplace_back(n);
            addToActiveNotes(&n.note);
            break;
          }
          else if (strippedStatus == 0x08 || (strippedStatus == 0x09 && me.data[2] == 0))  // Note Off
          {
            n.header.type = CLAP_EVENT_NOTE_OFF;
            n.header.size = sizeof(clap_event_note_t);
            n.note.port_index = 0;
            n.note.key = me.data[1] & 0x7F;
            n.note.velocity = (strippedStatus == 0x08) ? (float)(me.data[2] & 0x7F) / 127.0f : 0.0f;
            n.note.channel = channel;
            // Pair with the matching NOTE_ON's synthesized id (must run
            // before removeFromActiveNotes drops the active record).
            n.note.note_id = lookupNoteId(n.note.port_index, n.note.channel, n.note.key);

            _eventindices.emplace_back(_events.size());
            _events.emplace_back(n);
            removeFromActiveNotes(&n.note);
            break;
          }
          else if (strippedStatus == 0x0A)  // Poly Aftertouch → per-note PRESSURE
          {
            n.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            n.header.size = sizeof(clap_event_note_expression_t);
            n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
            n.noteexpression.port_index = 0;
            n.noteexpression.channel = channel;
            n.noteexpression.key = me.data[1] & 0x7F;
            n.noteexpression.note_id = lookupNoteId(n.noteexpression.port_index,
                                                    n.noteexpression.channel, n.noteexpression.key);
            n.noteexpression.value = (double)(me.data[2] & 0x7F) / 127.0;

            _eventindices.emplace_back(_events.size());
            _events.emplace_back(n);
            break;
          }
          else if (strippedStatus == 0x0D)  // Channel Pressure → channel-wide PRESSURE
          {
            n.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            n.header.size = sizeof(clap_event_note_expression_t);
            n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
            n.noteexpression.port_index = 0;
            n.noteexpression.channel = channel;
            n.noteexpression.key = -1;  // wildcard: all keys on this channel
            n.noteexpression.note_id = -1;
            n.noteexpression.value = (double)(me.data[1] & 0x7F) / 127.0;

            _eventindices.emplace_back(_events.size());
            _events.emplace_back(n);
            break;
          }
          else if (strippedStatus == 0x0E)  // Pitch Bend → channel-wide TUNING
          {
            n.header.type = CLAP_EVENT_NOTE_EXPRESSION;
            n.header.size = sizeof(clap_event_note_expression_t);
            n.noteexpression.expression_id = CLAP_NOTE_EXPRESSION_TUNING;
            n.noteexpression.port_index = 0;
            n.noteexpression.channel = channel;
            n.noteexpression.key = -1;  // wildcard: all keys on this channel
            n.noteexpression.note_id = -1;
            // MIDI pitch bend: 14-bit value (0-16383), center at 8192
            // Convert to CLAP semitones: ±2 semitones (MIDI default range)
            uint16_t bendValue = ((uint16_t)(me.data[2] & 0x7F) << 7) | (me.data[1] & 0x7F);
            n.noteexpression.value = ((double)bendValue - 8192.0) / 8192.0 * 2.0;

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
        n.header.time = sampleOffset;
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
                                          const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount,
                                          NSInteger outputBusNumber, AudioBufferList *outputData,
                                          const AURenderEvent *realtimeEventListHead,
                                          AURenderPullInputBlock __unsafe_unretained pullInputBlock)
{
  // Never let the plugin write past the storage sized at allocate time —
  // hosts (and auval) probing beyond maximumFramesToRender must get the
  // documented error, not a heap overrun.
  if (frameCount > _numMaxSamples)
  {
    return kAudioUnitErr_TooManyFramesToProcess;
  }

  // AUv3 calls the render block once per output bus. CLAP processes all buses
  // in a single process() call. All pulls of one render cycle share the same
  // timestamp, so we run the full CLAP process on the first bus pulled in a
  // cycle (whichever it is), storing all output. The other buses of the same
  // cycle just copy from storage.
  if (timestamp->mSampleTime == _lastProcessedSampleTime &&
      timestamp->mHostTime == _lastProcessedHostTime)
  {
    goto copyOutput;
  }
  _lastProcessedSampleTime = timestamp->mSampleTime;
  _lastProcessedHostTime = timestamp->mHostTime;

  // Clear events from previous cycle
  _events.clear();
  _eventindices.clear();

  // Deliver host parameter changes queued via queueParameterChange()
  // (AUParameter.setValue while rendering) at the top of this cycle.
  {
    QueuedParamChange qpc;
    while (_hostParamChanges.pop(qpc))
    {
      _hostParamChangesCount.fetch_sub(1);
      addParameterEvent(qpc.id, qpc.value, 0);
    }
  }

#if 1
  // Translate AUv3 events to CLAP events
  if (realtimeEventListHead)
  {
    AUEventSampleTime bufferStart = (AUEventSampleTime)timestamp->mSampleTime;
    translateAUv3Events(realtimeEventListHead, bufferStart, frameCount);
    PROCLOG("process: translated %zu events", _events.size());
  }
#endif
  // Sort events by timestamp (stable in arrival order for same-time ties).
  sortEventIndices();

  // Guard against the AU scheduler reordering a same-block NOTE_ON/NOTE_OFF
  // pair into NOTE_OFF-first order, which otherwise causes stuck notes in
  // plugins that silently drop orphan note-offs.
  reorderSameSampleOrphanOffs(frameCount);

  _processData.frames_count = frameCount;

  // Setup transport
  _transport.flags = 0;
  _transport.song_pos_beats = 0;
  _transport.song_pos_seconds = 0;
  _transport.tempo = 120;
  _transport.tempo_inc = 0;
  _transport.loop_start_beats = 0;
  _transport.loop_end_beats = 0;
  _transport.loop_start_seconds = 0;
  _transport.loop_end_seconds = 0;
  _transport.bar_start = 0;
  _transport.bar_number = 0;
  _transport.tsig_num = 4;
  _transport.tsig_denom = 4;
  _processData.steady_time = (int64_t)timestamp->mSampleTime;

  if (_transportStateBlock)
  {
    AUHostTransportStateFlags transportFlags = 0;
    double currentSamplePosition = 0;
    double cycleStartBeatPosition = 0;
    double cycleEndBeatPosition = 0;

    if (_transportStateBlock(&transportFlags, &currentSamplePosition, &cycleStartBeatPosition,
                             &cycleEndBeatPosition))
    {
      if (transportFlags & AUHostTransportStateMoving) _transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
      if (transportFlags & AUHostTransportStateRecording)
        _transport.flags |= CLAP_TRANSPORT_IS_RECORDING;
      if (transportFlags & AUHostTransportStateCycling)
      {
        _transport.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
        _transport.loop_start_beats = doubleToBeatTime(cycleStartBeatPosition);
        _transport.loop_end_beats = doubleToBeatTime(cycleEndBeatPosition);
      }
    }
  }

  if (_musicalContextBlock)
  {
    double tempo = 0;
    double tsigNum = 0;
    NSInteger tsigDenom = 0;
    double beatPos = 0;
    NSInteger sampleOffsetToNextBeat = 0;
    double downbeatPos = 0;

    if (_musicalContextBlock(&tempo, &tsigNum, &tsigDenom, &beatPos, &sampleOffsetToNextBeat,
                             &downbeatPos))
    {
      if (tempo > 0)
      {
        _transport.tempo = tempo;
        _transport.flags |= CLAP_TRANSPORT_HAS_TEMPO;
      }
      _transport.song_pos_beats = doubleToBeatTime(beatPos);
      _transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;

      if (tsigDenom > 0)
      {
        _transport.tsig_num = (uint16_t)tsigNum;
        _transport.tsig_denom = (uint16_t)tsigDenom;
        _transport.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
      }

      _transport.bar_start = doubleToBeatTime(downbeatPos);

      // Derive seconds position from beat position and tempo
      if (tempo > 0)
      {
        double seconds = beatPos * 60.0 / tempo;
        _transport.song_pos_seconds = doubleToSecTime(seconds);
        _transport.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
      }
    }
  }

  // Pull input audio
  PROCLOG("process: pulling input (_numInputs=%u, pullInputBlock=%{public}s)", _numInputs,
          pullInputBlock ? "yes" : "nil");
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
      PROCLOG("process: pulling bus %u (%u ch)", bus, numCh);
      AUAudioUnitStatus status =
          pullInputBlock(&pullFlags, timestamp, frameCount, bus, _inputBufferList);
      PROCLOG("process: pull bus %u status=%d", bus, (int)status);
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

  // Point all output ports to our internal storage so CLAP writes there
  for (uint32_t bus = 0; bus < _numOutputs; ++bus)
  {
    uint32_t numCh = _output_ports[bus].channel_count;
    for (uint32_t ch = 0; ch < numCh; ++ch)
    {
      _output_ports[bus].data32[ch] = _outputStorage[_outputStorageOffset[bus] + ch].data();
    }
  }

  // Process once for all buses
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
      case CLAP_EVENT_NOTE_EXPRESSION:
      {
        if (!midiOutputEventBlock) break;

        auto &ne = evt.noteexpression;

        if (ne.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && ne.key >= 0)
        {
          // Per-note pressure → Poly Aftertouch
          uint8_t data[3] = {(uint8_t)(0xA0 | (ne.channel >= 0 ? ne.channel & 0x0F : 0)),
                             (uint8_t)(ne.key & 0x7F),
                             (uint8_t)(std::clamp(ne.value, 0.0, 1.0) * 127.0)};
          midiOutputEventBlock(timestamp->mSampleTime + evt.header.time, 0, 3, data);
        }
        else if (ne.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && ne.key < 0)
        {
          // Channel-wide pressure → Channel Pressure
          uint8_t data[2] = {(uint8_t)(0xD0 | (ne.channel >= 0 ? ne.channel & 0x0F : 0)),
                             (uint8_t)(std::clamp(ne.value, 0.0, 1.0) * 127.0)};
          midiOutputEventBlock(timestamp->mSampleTime + evt.header.time, 0, 2, data);
        }
        else if (ne.expression_id == CLAP_NOTE_EXPRESSION_TUNING)
        {
          // Tuning → Pitch Bend (±2 semitone range)
          double normalized = std::clamp(ne.value / 2.0, -1.0, 1.0);
          uint16_t bendValue = (uint16_t)((normalized + 1.0) * 8192.0);
          if (bendValue > 16383) bendValue = 16383;
          uint8_t data[3] = {(uint8_t)(0xE0 | (ne.channel >= 0 ? ne.channel & 0x0F : 0)),
                             (uint8_t)(bendValue & 0x7F), (uint8_t)((bendValue >> 7) & 0x7F)};
          midiOutputEventBlock(timestamp->mSampleTime + evt.header.time, 0, 3, data);
        }
        // Other expression types (volume, pan, vibrato, brightness) have no MIDI 1.0 equivalent
        break;
      }
      default:
        break;
    }
  }
  _outevents.clear();

copyOutput:
  // Hand the stored output to the host for this bus. A null mData is the
  // host asking the AU to provide its own buffer (auval exercises this) —
  // point it at our storage instead of skipping the channel.
  if (outputData && outputBusNumber >= 0 && outputBusNumber < (NSInteger)_numOutputs)
  {
    uint32_t outBus = (uint32_t)outputBusNumber;
    uint32_t numCh = std::min((uint32_t)outputData->mNumberBuffers, _output_ports[outBus].channel_count);
    for (uint32_t ch = 0; ch < numCh; ++ch)
    {
      auto &storage = _outputStorage[_outputStorageOffset[outBus] + ch];
      if (outputData->mBuffers[ch].mData == nullptr)
      {
        outputData->mBuffers[ch].mData = storage.data();
      }
      else
      {
        memcpy(outputData->mBuffers[ch].mData, storage.data(), frameCount * sizeof(float));
      }
      outputData->mBuffers[ch].mDataByteSize = frameCount * sizeof(float);
    }
  }

  return noErr;
}

void ProcessAdapter::queueParameterChange(clap_id paramId, double value)
{
  // fixedqueue wraps silently — a wrapped ring reads back as EMPTY,
  // discarding everything. Producers are serialized, so the count check
  // is race-free against other producers; dropping the newest change on a
  // >4095-entry burst between two render cycles is the lesser evil (the
  // wrapper's value cache already holds the latest value).
  if (_hostParamChangesCount.load() >= kHostParamQueueSize - 1) return;
  _hostParamChanges.push({paramId, value});
  _hostParamChangesCount.fetch_add(1);
}

bool ProcessAdapter::dequeueParameterChange(QueuedParamChange &out)
{
  if (!_hostParamChanges.pop(out)) return false;
  _hostParamChangesCount.fetch_sub(1);
  return true;
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
  {
    auto it = _cookieCache.find(paramId);
    if (it != _cookieCache.end()) n.param.cookie = it->second;
  }
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

void ProcessAdapter::addToActiveNotes(const clap_event_note_t *note)
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

void ProcessAdapter::removeFromActiveNotes(const clap_event_note_t *note)
{
  for (auto &i : _activeNotes)
  {
    if (i.used && i.port_index == note->port_index && i.channel == note->channel && i.key == note->key)
    {
      i.used = false;
    }
  }
}

int32_t ProcessAdapter::synthesizeNoteId()
{
  // Monotonic counter. clap_event_note_t::note_id is int32_t and -1 means
  // wildcard; keep the synthesized value non-negative. Wrap-around at
  // INT32_MAX is implausible (~2 billion notes per activation cycle), but
  // we reset to 0 on overflow rather than emit a negative ID.
  if (_nextNoteId < 0) _nextNoteId = 0;
  return _nextNoteId++;
}

int32_t ProcessAdapter::lookupNoteId(int16_t port_index, int16_t channel, int16_t key) const
{
  for (const auto &i : _activeNotes)
  {
    if (i.used && i.port_index == port_index && i.channel == channel && i.key == key) return i.note_id;
  }
  return -1;
}

}  // namespace Clap::AUv3

#pragma clang diagnostic pop
