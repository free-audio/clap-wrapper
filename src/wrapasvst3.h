#pragma once
/*
		CLAP as VST3

		this VST3 opens a CLAP plugin and matches all corresponding VST3 calls to it.

		(c) 2022 Defiant Nerd

*/

#include "clap_proxy.h"
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"
#include "detail/vst3/plugview.h"
#include "wrapasvst3_version.h"

using namespace Steinberg;

struct ClapHostExtensions;
class ProcessAdapter;

class ClapAsVst3 : public Steinberg::Vst::SingleComponentEffect
	, public Clap::IHost
{
public:
	using super = Steinberg::Vst::SingleComponentEffect;

	static FUnknown* createInstance(void* context);

	ClapAsVst3(Clap::Library* lib, int number, void* context) : super(), _library(lib), _libraryIndex(number), _creationcontext(context) {}

	//---from IComponent-----------------------
	tresult PLUGIN_API initialize(FUnknown* context) override;
	tresult PLUGIN_API terminate() override;
	tresult PLUGIN_API setActive(TBool state) override;
	tresult PLUGIN_API process(Vst::ProcessData& data) override;
	tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override;
	tresult PLUGIN_API setState(IBStream* state) override;
	tresult PLUGIN_API getState(IBStream* state) override;
	tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& newSetup) override;
	tresult PLUGIN_API setProcessing(TBool state) override;
	tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns,
		Vst::SpeakerArrangement* outputs,
		int32 numOuts) override;

	uint32  PLUGIN_API getTailSamples() override;

	tresult PLUGIN_API getControllerClassId(TUID classID) override 
	{ 
		// TUID lcid = INLINE_UID_FROM_FUID(ClapAsVst3UID); classid = lcid; return kResultOk; 
		if (ClapAsVst3UID.isValid())
		{
			ClapAsVst3UID.toTUID(classID);
			return kResultTrue;
		}
		return kResultFalse;
	}

	//----from IEditControllerEx1--------------------------------
	IPlugView* PLUGIN_API createView(FIDString name) override;

	//---Interface---------
	OBJ_METHODS(ClapAsVst3, SingleComponentEffect)
		tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override;
	REFCOUNT_METHODS(SingleComponentEffect);

	// Clap::IHost
	void setupAudioBusses(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* audioports) override;
	void setupMIDIBusses(const clap_plugin_t* plugin, const clap_plugin_note_ports_t* noteports) override;
  void setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params) override;

	void param_rescan(clap_param_rescan_flags flags) override;
	void param_clear(clap_id param, clap_param_clear_flags flags) override;
	void param_request_flush() override;

	void mark_dirty() override;

	void schnick() override;

private:
	// helper functions
	void addAudioBusFrom(const clap_audio_port_info_t* info, bool is_input);
	void addMIDIBusFrom(const clap_note_port_info_t* info, bool is_input);
	Clap::Library* _library = nullptr;
	int _libraryIndex = 0;
	std::shared_ptr<Clap::Plugin> _plugin;
	ClapHostExtensions* _hostextensions = nullptr;
	ProcessAdapter* processAdapter = nullptr;

	void* _creationcontext;																		// context from the CLAP library

	// plugin state
	bool _active = false;
	bool _processing = false;

};
