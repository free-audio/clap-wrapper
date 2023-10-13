#pragma once

/*
    AUv2 process Adapter
 
    Copyright (c) 2023 Timo Kaluza (defiantnerd)
 
    This file i spart of the clap-wrappers project which is released under MIT License
 
 See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.
 
 
 The process adapter is responsible to translate events, timing information
 
 */

#include <clap/clap.h>
#include "clap_proxy.h"
#include <clap/helpers/plugin-proxy.hxx>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <AudioUnitSDK/AUBase.h>

// TODO: check if additional AU headers are needed
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnitParameters.h>
#include <AudioToolbox/AudioUnitUtilities.h>
#include <AudioUnit/AUComponent.h>
#include "../clap/automation.h"
#include "parameter.h"
#include <map>

namespace Clap::AUv2
{
using ParameterTree = std::map<uint32_t, std::unique_ptr<Clap::AUv2::Parameter>>;

struct ProcessData
{
  AudioUnitRenderActionFlags& flags;
  const AudioTimeStamp& timestamp;
  uint32_t numSamples = 0;
  void* audioUnit = nullptr;

  // -------------
  bool _AUtransportValid;  // true if:
  // information from the AU Host
  Float64 _cycleStart;
  Float64 _cycleEnd;
  Float64 _currentSongPosInSeconds;  // outCurrentSampleInTimeLine

  Boolean _isPlaying;
  Boolean _transportChanged;
  Boolean _isLooping;

  // --------------
  bool _AUbeatAndTempoValid;  // true if:
  // information from the AU host
  Float64 _beat;
  Float64 _tempo;

  // --------------
  bool _AUmusicalTimeValid;  // true if:
  // information from the AU host
  UInt32 _offsetToNextBeat;
  Float32 _musicalNumerator;
  UInt32 _musicalDenominator;
  Float64 _currentDownBeat;
};

class ProcessAdapter
{
 public:
  ProcessAdapter(const Clap::PluginProxy& pluginProxy) : _pluginProxy{pluginProxy}
  {
  }

  typedef union clap_multi_event
  {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_midi_t midi;
    clap_event_midi_sysex_t sysex;
    clap_event_param_value_t param;
    clap_event_note_expression_t noteexpression;
  } clap_multi_event_t;

  void setupProcessing(ausdk::AUScope& audioInputs, ausdk::AUScope& audioOutputs,
                       Clap::IAutomation* automationInterface, ParameterTree* parameters,

                       uint32_t numMaxSamples, uint32_t preferredMIDIDialect);

  void process(ProcessData& data);  // AU Data
  void flush();

  // interface for AUv2 wrapper:
  void addMIDIEvent(UInt32 inStatus, UInt32 inData1, UInt32 inData2, UInt32 inOffsetSampleFrame);
  void addParameterEvent(const clap_param_info_t& info, double value, uint32_t inOffsetSampleFrame);
  // void startNote()
  ~ProcessAdapter();

 private:
  // necessary C callbacks:
  static uint32_t input_events_size(const struct clap_input_events* list);
  static const clap_event_header_t* input_events_get(const struct clap_input_events* list,
                                                     uint32_t index);

  static bool output_events_try_push(const struct clap_output_events* list,
                                     const clap_event_header_t* event);

  void sortEventIndices();

  bool enqueueOutputEvent(const clap_event_header_t* event);

  void processOutputEvents();

  void addToActiveNotes(const clap_event_note* note);
  void removeFromActiveNotes(const clap_event_note* note);

  // the plugin
  const Clap::PluginProxy& _pluginProxy;

  // for automation gestures
  std::vector<clap_id> _gesturedParameters;

  // for INoteExpression
  struct ActiveNote
  {
    bool used = false;
    int32_t note_id;  // -1 if unspecified, otherwise >=0
    int16_t port_index;
    int16_t channel;  // 0..15
    int16_t key;      // 0..127
  };
  std::vector<ActiveNote> _activeNotes;

  uint32_t _numInputs = 0;
  uint32_t _numOutputs = 0;

  clap_audio_buffer_t* _input_ports = nullptr;
  clap_audio_buffer_t* _output_ports = nullptr;
  clap_event_transport_t _transport = {};
  clap_input_events_t _in_events = {};
  clap_output_events_t _out_events = {};

  float* _silent_input = nullptr;
  float* _silent_output = nullptr;

  clap_process_t _processData = {-1, 0, &_transport, nullptr, nullptr, 0, 0, &_in_events, &_out_events};

  std::vector<clap_multi_event_t> _events;
  std::vector<size_t> _eventindices;

  std::vector<clap_multi_event_t> _outevents;

  uint32_t _preferred_midi_dialect = CLAP_NOTE_DIALECT_CLAP;

  Clap::IAutomation* _automation = nullptr;
  ParameterTree* _parameters = nullptr;

  // AU Process Data?
  ausdk::AUScope* _audioInputScope = nullptr;
  ausdk::AUScope* _audioOutputScope = nullptr;
};
}  // namespace Clap::AUv2
