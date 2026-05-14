#pragma once

/*
    AUv3 Process Adapter

    Copyright (c) 2024 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    The AUv3 process adapter translates between the AUv3 render block model
    (AURenderEvent linked list, AURenderPullInputBlock) and the CLAP process model
    (clap_process_t with event lists and audio buffers).
*/

#include <clap/clap.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"

#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#include <vector>
#include <map>
#include <unordered_map>
#include "../clap/automation.h"

namespace Clap::AUv3
{

typedef union clap_multi_event
{
  clap_event_header_t header;
  clap_event_note_t note;
  clap_event_midi_t midi;
  clap_event_midi_sysex_t sysex;
  clap_event_param_value_t param;
  clap_event_note_expression_t noteexpression;
} clap_multi_event_t;

class ProcessAdapter
{
 public:
  ProcessAdapter() = default;
  ~ProcessAdapter();

  // Set up the processing state for the given bus configuration.
  // Called once when render resources are allocated.
  void setupProcessing(uint32_t numInputBusses, const uint32_t *inputChannelCounts,
                       uint32_t numOutputBusses, const uint32_t *outputChannelCounts,
                       const clap_plugin_t *plugin, const clap_plugin_params_t *ext_params,
                       Clap::IAutomation *automation, uint32_t numMaxSamples,
                       uint32_t preferredMIDIDialect);

  // Main render call - invoked from the AUv3 internalRenderBlock.
  // Translates AUv3 events, pulls input, calls CLAP process, and writes output.
  AUAudioUnitStatus process(AudioUnitRenderActionFlags *actionFlags, const AudioTimeStamp *timestamp,
                            AVAudioFrameCount frameCount, NSInteger outputBusNumber,
                            AudioBufferList *outputData, const AURenderEvent *realtimeEventListHead,
                            AURenderPullInputBlock __unsafe_unretained pullInputBlock);

  // Provide transport/musical context from the host
  void setTransportStateBlock(AUHostTransportStateBlock __nullable block);
  void setMusicalContextBlock(AUHostMusicalContextBlock __nullable block);

  // Queue a parameter change from the host (outside render block)
  void addParameterEvent(clap_id paramId, double value, uint32_t sampleOffset);

  // MIDI output event block (set by the AU host)
  AUMIDIOutputEventBlock __nullable midiOutputEventBlock;

 private:
  static uint32_t input_events_size(const struct clap_input_events *list);
  static const clap_event_header_t *input_events_get(const struct clap_input_events *list,
                                                     uint32_t index);
  static bool output_events_try_push(const struct clap_output_events *list,
                                     const clap_event_header_t *event);

  void sortEventIndices();
  bool enqueueOutputEvent(const clap_event_header_t *event);
  void translateAUv3Events(const AURenderEvent *head, AUEventSampleTime bufferStartTime,
                           AVAudioFrameCount frameCount);

  // Post-sort pass that guards against the AU scheduler reordering
  // same-block NOTE_ON/NOTE_OFF pairs. See process.mm for the full
  // rationale and behaviour.
  void reorderSameSampleOrphanOffs(AVAudioFrameCount frameCount);

 public:
  const std::unordered_map<clap_id, void *> *_cookieCache = nullptr;

 private:
  const clap_plugin_t *_plugin = nullptr;
  const clap_plugin_params_t *_ext_params = nullptr;
  Clap::IAutomation *_automation = nullptr;

  uint32_t _numInputs = 0;
  uint32_t _numOutputs = 0;

  clap_audio_buffer_t *_input_ports = nullptr;
  clap_audio_buffer_t *_output_ports = nullptr;
  clap_event_transport_t _transport = {};
  clap_input_events_t _in_events = {};
  clap_output_events_t _out_events = {};

  float *_silent_input = nullptr;
  float *_silent_output = nullptr;

  clap_process_t _processData = {-1, 0, &_transport, nullptr, nullptr, 0, 0, &_in_events, &_out_events};

  std::vector<clap_multi_event_t> _events;
  std::vector<size_t> _eventindices;
  std::vector<clap_multi_event_t> _outevents;

  uint32_t _preferred_midi_dialect = CLAP_NOTE_DIALECT_CLAP;

  // Active note tracking for note expression targeting
  struct ActiveNote
  {
    bool used = false;
    int32_t note_id;
    int16_t port_index;
    int16_t channel;
    int16_t key;
  };
  std::vector<ActiveNote> _activeNotes;

  void addToActiveNotes(const clap_event_note_t *note);
  void removeFromActiveNotes(const clap_event_note_t *note);

  // AUv3 MIDI events carry no per-note identifier, so the wrapper synthesizes
  // monotonically increasing note_ids at NOTE_ON time and pairs them back
  // with the matching NOTE_OFF / per-key expressions via the active-notes
  // shadow. Plugins that key voice tracking off note_id (rather than the
  // (channel, key) pair) get a usable identity instead of -1 everywhere.
  int32_t _nextNoteId = 0;
  int32_t synthesizeNoteId();
  int32_t lookupNoteId(int16_t port_index, int16_t channel, int16_t key) const;

  AUHostTransportStateBlock __nullable _transportStateBlock = nil;
  AUHostMusicalContextBlock __nullable _musicalContextBlock = nil;

  // Temporary storage for input pulling
  AudioBufferList *_inputBufferList = nullptr;
  uint32_t _inputBufferListChannels = 0;

  // Multi-bus render tracking: AUv3 calls the render block once per output bus,
  // but CLAP processes all buses in a single process() call. We process on the
  // first bus and just copy output for subsequent buses in the same cycle.
  uint64_t _lastProcessedSampleTime = UINT64_MAX;
  uint32_t _numMaxSamples = 0;
  std::vector<std::vector<float>> _outputStorage;  // [bus * maxCh + ch][samples]
};

}  // namespace Clap::AUv3

#pragma clang diagnostic pop
