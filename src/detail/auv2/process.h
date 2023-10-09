#pragma once

/*
    AUv2 process Adapter
 
    Copyright (c) 2023 Timo Kaluza (defiantnerd)
 
    This file i spart of the clap-wrappers project which is released under MIT License
 
 See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.
 
 
 The process adapter is responible to translate events, timing information
 
 */

#include <clap/clap.h>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <AudioUnitSDK/AUBase.h>

// TODO: check if additional AU headers are needed
//#include <AudioToolbox/AudioToolbox.h>
//#include <AudioUnit/AudioUnitParameters.h>

namespace Clap::AUv2
{
  class ProcessAdapter
{
public:
  typedef union clap_multi_event
  {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_midi_t midi;
    clap_event_midi_sysex_t sysex;
    clap_event_param_value_t param;
    clap_event_note_expression_t noteexpression;
  } clap_multi_event_t;

  void setupProcessing(const clap_plugin_t* plugin, const clap_plugin_params_t* ext_params, uint32_t numMaxSamples);
  
  void process(); // AU Data
  void flush();
  
  // necessary C callbacks:
  static uint32_t input_events_size(const struct clap_input_events* list);
  static const clap_event_header_t* input_events_get(const struct clap_input_events* list,
                                                     uint32_t index);

  static bool output_events_try_push(const struct clap_output_events* list,
                                     const clap_event_header_t* event);
private:
  void sortEventIndices();
  
  bool enqueueOutputEvent(const clap_event_header_t* event);
  void addToActiveNotes(const clap_event_note* note);
  void removeFromActiveNotes(const clap_event_note* note);
  
  // the plugin
  const clap_plugin_t* _plugin = nullptr;
  const clap_plugin_params_t* _ext_params = nullptr;

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
  
  // AU Process Data?
};
}

