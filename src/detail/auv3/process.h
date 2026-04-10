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
  AUAudioUnitStatus process(AudioUnitRenderActionFlags *actionFlags,
                            const AudioTimeStamp *timestamp, AVAudioFrameCount frameCount,
                            NSInteger outputBusNumber, AudioBufferList *outputData,
                            const AURenderEvent *realtimeEventListHead,
                            AURenderPullInputBlock __unsafe_unretained pullInputBlock);

  // Provide transport state from the host
  void setTransportStateBlock(AUHostTransportStateBlock __nullable block);

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
  void translateAUv3Events(const AURenderEvent *head);

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

  AUHostTransportStateBlock __nullable _transportStateBlock = nil;

  // Temporary storage for input pulling
  AudioBufferList *_inputBufferList = nullptr;
  uint32_t _inputBufferListChannels = 0;
};

}  // namespace Clap::AUv3

#pragma clang diagnostic pop
