#pragma once
/*
		CLAP as VST3

		this VST3 opens a CLAP plugin and matches all corresponding VST3 calls to it.

		(c) 2022 Defiant Nerd

*/

#include "clap_proxy.h"
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <public.sdk/source/vst/vstsinglecomponenteffect.h>
#include <public.sdk/source/vst/vstnoteexpressiontypes.h>
#include "detail/vst3/plugview.h"
#include "detail/os/osutil.h"
#include "detail/clap/automation.h"
#include <mutex>

using namespace Steinberg;

struct ClapHostExtensions;
namespace Clap
{
	class ProcessAdapter;
}

class queueEvent
{
public:
	typedef enum class type
	{
		editstart,
		editvalue,
		editend,
	} type_t;
	type_t _type;
	union
	{
		clap_id _id;
		clap_event_param_value_t _value;
	} _data;
};

class beginEvent : public queueEvent
{
public:
	beginEvent(clap_id id)
		: queueEvent()
	{
		this->_type = type::editstart;
		_data._id = id;
	}
};

class endEvent : public queueEvent
{
public:
	endEvent(clap_id id)
		: queueEvent()
	{
		this->_type = type::editend;
		_data._id = id;
	}
};

class valueEvent : public queueEvent
{
public:
	valueEvent(const clap_event_param_value_t* value)
		: queueEvent()
	{
		_type = type::editvalue;
		_data._value = *value;
	}
};

class ClapAsVst3 : public Steinberg::Vst::SingleComponentEffect
	, public Steinberg::Vst::IMidiMapping
	, public Steinberg::Vst::INoteExpressionController
	, public Clap::IHost
	, public Clap::IAutomation
	, public os::IPlugObject
{
public:

	using super = Steinberg::Vst::SingleComponentEffect;

	static FUnknown* createInstance(void* context);

	ClapAsVst3(Clap::Library* lib, int number, void* context) 
		: super()
		, Steinberg::Vst::IMidiMapping()
 		, Steinberg::Vst::INoteExpressionController()
		, _library(lib)
		, _libraryIndex(number)
		, _creationcontext(context) {}

	//---from IComponent-----------------------
	tresult PLUGIN_API initialize(FUnknown* context) override;
	tresult PLUGIN_API terminate() override;
	tresult PLUGIN_API setActive(TBool state) override;
	tresult PLUGIN_API process(Vst::ProcessData& data) override;
	tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override;
	tresult PLUGIN_API setState(IBStream* state) override;
	tresult PLUGIN_API getState(IBStream* state) override;
	uint32  PLUGIN_API getLatencySamples() override;
	uint32  PLUGIN_API getTailSamples() override;
	tresult PLUGIN_API setupProcessing(Vst::ProcessSetup& newSetup) override;
	tresult PLUGIN_API setProcessing(TBool state) override;
	tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns,
		Vst::SpeakerArrangement* outputs,
		int32 numOuts) override;


	//----from IEditControllerEx1--------------------------------
	IPlugView* PLUGIN_API createView(FIDString name) override;

	//----from IMidiMapping--------------------------------------
	tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel,
		Vst::CtrlNumber midiControllerNumber, Vst::ParamID& id/*out*/) override;

#if 1
	//----from INoteExpressionController-------------------------
		/** Returns number of supported note change types for event bus index and channel. */
	int32 PLUGIN_API getNoteExpressionCount(int32 busIndex, int16 channel) override;

	/** Returns note change type info. */
	tresult PLUGIN_API getNoteExpressionInfo(int32 busIndex, int16 channel, int32 noteExpressionIndex, Vst::NoteExpressionTypeInfo& info /*out*/) override;

	/** Gets a user readable representation of the normalized note change value. */
	tresult PLUGIN_API getNoteExpressionStringByValue(int32 busIndex, int16 channel, Vst::NoteExpressionTypeID id, Vst::NoteExpressionValue valueNormalized /*in*/, Vst::String128 string /*out*/) override;

	/** Converts the user readable representation to the normalized note change value. */
	tresult PLUGIN_API getNoteExpressionValueByString(int32 busIndex, int16 channel, Vst::NoteExpressionTypeID id, const Vst::TChar* string /*in*/, Vst::NoteExpressionValue& valueNormalized /*out*/ ) override;
#endif
	//---Interface--------------------------------------------------------------------------
	OBJ_METHODS(ClapAsVst3, SingleComponentEffect)
	DEFINE_INTERFACES
	DEF_INTERFACE(IMidiMapping)
	DEF_INTERFACE(INoteExpressionController)
	// tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override;
	END_DEFINE_INTERFACES(SingleComponentEffect)
	REFCOUNT_METHODS(SingleComponentEffect);
	


	//---Clap::IHost------------------------------------------------------------------------

	void setupWrapperSpecifics(const clap_plugin_t* plugin) override;

	void setupAudioBusses(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* audioports) override;
	void setupMIDIBusses(const clap_plugin_t* plugin, const clap_plugin_note_ports_t* noteports) override;
  void setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params) override;

	void param_rescan(clap_param_rescan_flags flags) override;
	void param_clear(clap_id param, clap_param_clear_flags flags) override;
	void param_request_flush() override;

	bool gui_can_resize() override;
	bool gui_request_resize(uint32_t width, uint32_t height) override;
	bool gui_request_show() override;
	bool gui_request_hide() override;

	void latency_changed() override;

	void tail_changed() override;

	void mark_dirty() override;

	void restartPlugin() override;

	void request_callback() override;

	// clap_timer support
	bool register_timer(uint32_t period_ms, clap_id* timer_id) override;
	bool unregister_timer(clap_id timer_id) override;

	//----from IPlugObject
	void onIdle() override;

	// from Clap::IAutomation
	void onBeginEdit(clap_id id) override;
	void onPerformEdit(const clap_event_param_value_t* value) override;
	void onEndEdit(clap_id id) override;

private:
	// helper functions
	void addAudioBusFrom(const clap_audio_port_info_t* info, bool is_input);
	void addMIDIBusFrom(const clap_note_port_info_t* info, uint32_t index, bool is_input);

	Vst::UnitID getUnitInfo(const char* modulename);
	std::map<std::string, Vst::UnitID> _moduleToUnit;

	Clap::Library* _library = nullptr;
	int _libraryIndex = 0;
	std::shared_ptr<Clap::Plugin> _plugin;
	ClapHostExtensions* _hostextensions = nullptr;
	clap_plugin_as_vst3_t* _vst3specifics = nullptr;
	Clap::ProcessAdapter* _processAdapter = nullptr;
	WrappedView* _wrappedview = nullptr;

	void* _creationcontext;																		// context from the CLAP library

	// plugin state
	bool _active = false;
	bool _processing = false;
	std::mutex _processingLock;
	std::atomic_bool _requestedFlush = false;
	std::atomic_bool _requestUICallback = false;

	// the queue from audiothread to UI thread
	util::fixedqueue<queueEvent, 8192> _queueToUI;

	// for IMidiMapping
	Vst::ParamID _IMidiMappingIDs[Vst::ControllerNumbers::kCountCtrlNumber] = { 0 };
	bool _IMidiMappingEasy = true;
	uint8_t _numMidiChannels = 16;

	// for timer
	struct TimerObject
	{
		uint32_t period = 0;		// if period is 0 the entry is unused (and can be reused)
		uint64_t nexttick = 0;
		clap_id timer_id = 0;
	};
	std::vector<TimerObject> _timersObjects;

	// INoteExpression
	Vst::NoteExpressionTypeContainer _noteExpressions;
	uint32_t _expressionmap = clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_PRESSURE;
};
