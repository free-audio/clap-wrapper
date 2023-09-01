#pragma once

/*
	  VST3 process adapter

		Copyright (c) 2022 Timo Kaluza (defiantnerd)

		This file is part of the clap-wrappers project which is released under MIT License.
		See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.


		The process adapter is responible to translate events, timing information 

*/
#include <clap/clap.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <public.sdk/source/vst/vstparameters.h>
#include <public.sdk/source/vst/vstbus.h>
#include <vector>
#include <memory>

#include "../clap/automation.h"

namespace Clap
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

		void setupProcessing(const clap_plugin_t* plugin, const clap_plugin_params_t* ext_params, 
			Steinberg::Vst::BusList& numInputs, Steinberg::Vst::BusList& numOutputs,
			uint32_t numSamples, size_t numEventInputs, size_t numEventOutputs,
         Steinberg::Vst::ParameterContainer& params, Steinberg::Vst::IComponentHandler* componenthandler,
         IAutomation* automation, bool enablePolyPressure, bool supportsTuningNoteExpression);
		void process(Steinberg::Vst::ProcessData& data);
		void flush();
		void processOutputParams(Steinberg::Vst::ProcessData& data);
		void activateAudioBus(Steinberg::Vst::BusDirection dir, Steinberg::int32 index, Steinberg::TBool state);

		// C callbacks
		static uint32_t input_events_size(const struct clap_input_events* list);
		static const clap_event_header_t* input_events_get(const struct clap_input_events* list, uint32_t index);

		static bool output_events_try_push(const struct clap_output_events* list, const clap_event_header_t* event);		
	private:
		void sortEventIndices();
		void processInputEvents(Steinberg::Vst::IEventList* eventlist);

		bool enqueueOutputEvent(const clap_event_header_t* event);
		void addToActiveNotes(const clap_event_note* note);
		void removeFromActiveNotes(const clap_event_note* note);

		// the plugin
		const clap_plugin_t* _plugin = nullptr;
		const clap_plugin_params_t* _ext_params = nullptr;

		Steinberg::Vst::ParameterContainer* parameters = nullptr;
		Steinberg::Vst::IComponentHandler* _componentHandler = nullptr;
		IAutomation* _automation = nullptr;
		Steinberg::Vst::BusList* _audioinputs = nullptr;
		Steinberg::Vst::BusList* _audiooutputs = nullptr;

		// for automation gestures
		std::vector<clap_id> _gesturedParameters;

		// for INoteExpression
		struct ActiveNote
		{
			bool used = false;
			int32_t note_id; // -1 if unspecified, otherwise >=0
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

		clap_process_t _processData = { -1, 0, &_transport, nullptr, nullptr, 0, 0, &_in_events, &_out_events };


		Steinberg::Vst::ProcessData* _vstdata = nullptr;

		std::vector<clap_multi_event_t> _events;
		std::vector<size_t> _eventindices;

		bool _supportsPolyPressure = false;
      bool _supportsTuningNoteExpression = false;

	};

}