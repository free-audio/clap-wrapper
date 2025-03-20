#pragma once

/*
    VST3 process adapter

    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.


    The process adapter is responible to translate events, timing information

*/

#include <clap/clap.h>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include <vector>
#include <memory>

namespace Clap::AAX
{
struct AAXProcessContext
{
  float* const* inputs;
  float* const* outputs;
  int32_t frames;
};

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

#if 0
		// the bitly helpers. These names conflict with macOS params.h but are
      // unused, so for now comment out
		static void setbit(uint64_t& val, int bit, bool value)
		{
			if (value) val |= ((uint64_t)1 << bit); else val &= ~((uint64_t)1 << bit);
		}
		static void setbits(uint64_t& val, int pos, int nbits, bool value)
		{
			uint64_t mask = (((uint64_t)1 << nbits) - 1);
			val &= ~(mask << pos);
			if (value) val |= mask << pos;
		}
#endif

  void setupProcessing(const clap_plugin_t* plugin, const clap_plugin_params_t* ext,
                       uint32_t maxFrames) noexcept;
  void process(const AAXProcessContext& ctx) noexcept;

 private:
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

  clap_event_transport_t _transport = {};
  clap_input_events_t _in_events = {};
  clap_output_events_t _out_events = {};

  clap_process_t _processData = {-1, 0, &_transport, nullptr, nullptr, 0, 0, &_in_events, &_out_events};

  std::vector<clap_multi_event_t> _events;
  std::vector<size_t> _eventindices;
};

}  // namespace Clap::AAX
