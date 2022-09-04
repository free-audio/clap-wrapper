#pragma once

#include <clap/clap.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <public.sdk/source/vst/vstparameters.h>
#include <vector>

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
  } clap_multi_event_t;

  void setupProcessing(size_t numInputs, size_t numOutputs, size_t numEventInputs, size_t numEventOutputs, Steinberg::Vst::ParameterContainer& params, Steinberg::Vst::IComponentHandler* componenthandler);
  void process(Steinberg::Vst::ProcessData& data, const clap_plugin_t* plugin);
	void processOutputParams(Steinberg::Vst::ProcessData& data, const clap_plugin_t* plugin);

	// C callbacks
	static uint32_t input_events_size(const struct clap_input_events* list);
	static const clap_event_header_t* input_events_get(const struct clap_input_events* list, uint32_t index);

	static bool output_events_try_push(const struct clap_output_events* list, const clap_event_header_t* event);
private:
	bool enqueueOutputEvent(const clap_event_header_t* event);
	Steinberg::Vst::ParameterContainer* parameters = nullptr;
	Steinberg::Vst::IComponentHandler* _componentHandler = nullptr;

	clap_audio_buffer_t _inputs;
	clap_audio_buffer_t _outputs;
	clap_event_transport_t _transport;
	clap_input_events_t _in_events;
	clap_output_events_t _out_events;

	clap_process_t _processData = { -1, 0, &_transport, &_inputs, &_outputs, 0, 0, &_in_events, &_out_events };

	Steinberg::Vst::ProcessData* _vstdata = nullptr;

	std::vector<clap_multi_event_t> _events;
	std::vector<size_t> _eventindices;
};
