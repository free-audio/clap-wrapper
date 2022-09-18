#include "wrapasvst3.h"
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstevents.h>
#include "detail/vst3/state.h"
#include "detail/vst3/process.h"
#include "detail/vst3/parameter.h"

// Clap::Library gClapLibrary;

struct ClapHostExtensions
{
	static inline ClapAsVst3* self(const clap_host_t* host)
	{
		return static_cast<ClapAsVst3*>(host->host_data);
	}
	static void mark_dirty(const clap_host_t* host) { self(host)->mark_dirty(); }
	const clap_host_state_t _state = { mark_dirty};



};


tresult PLUGIN_API ClapAsVst3::initialize(FUnknown* context)
{	
	auto result = super::initialize(context);
	if (result == kResultOk)
	{
		if (!_plugin)
		{
			_plugin = Clap::Plugin::createInstance(*_library, _libraryIndex, this);
		}
		result = (_plugin->initialize()) ? kResultOk : kResultFalse;
	}
	if (_plugin)
	{

	}
	return result;
}

tresult PLUGIN_API ClapAsVst3::terminate()
{
	if (_plugin)
	{
		_plugin->terminate();
		_plugin.reset();
	}
	return super::terminate();
}

tresult PLUGIN_API ClapAsVst3::setActive(TBool state)
{
	if (state)
	{
		if (_active)
			return kResultFalse;
		if (!_plugin->activate())
			return kResultFalse;
		_active = true;
		_processAdapter = new Clap::ProcessAdapter();
		os::attach(this);
	}	
	if (!state)
	{
		os::detach(this);
		if (_active)
		{
			_plugin->deactivate();
		}
		_active = false;
		delete _processAdapter;
		_processAdapter = nullptr;
	}
	return super::setActive(state);
}

tresult PLUGIN_API ClapAsVst3::process(Vst::ProcessData& data)
{
	this->_processAdapter->process(data, _plugin->_plugin);
	
  return kResultOk;
}

tresult PLUGIN_API ClapAsVst3::canProcessSampleSize(int32 symbolicSampleSize)
{
	if (symbolicSampleSize != Steinberg::Vst::kSample32)
	{
		return kResultFalse;
	}
	return kResultOk;
}

tresult PLUGIN_API ClapAsVst3::setState(IBStream* state)
{
	return ( _plugin->load(CLAPVST3StreamAdapter(state)) ? Steinberg::kResultOk : Steinberg::kResultFalse);
}

tresult PLUGIN_API ClapAsVst3::getState(IBStream* state)
{
	return ( _plugin->save(CLAPVST3StreamAdapter(state)) ? Steinberg::kResultOk : Steinberg::kResultFalse);
}
tresult PLUGIN_API ClapAsVst3::setupProcessing(Vst::ProcessSetup& newSetup) 
{
	if (newSetup.symbolicSampleSize != Vst::kSample32)
	{
		return kResultFalse;
	}
	_plugin->setSampleRate(newSetup.sampleRate);
	_plugin->setBlockSizes(newSetup.maxSamplesPerBlock, newSetup.maxSamplesPerBlock);	

	return kResultOk;
}
tresult PLUGIN_API ClapAsVst3::setProcessing(TBool state)
{
	if (state)
	{
		_processing = true;
		_processAdapter->setupProcessing(this->audioInputs.size(), this->audioOutputs.size(), this->eventInputs.size(), this->eventOutputs.size(), parameters, componentHandler, this);
		return (_plugin->start_processing() ? Steinberg::kResultOk : Steinberg::kResultFalse);
	}
	else
	{
		_processing = false;
		_plugin->stop_processing();
		return kResultOk;
	}
}
tresult PLUGIN_API ClapAsVst3::setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns,
	Vst::SpeakerArrangement* outputs,
	int32 numOuts)
{
	return kResultOk;
};

uint32 PLUGIN_API ClapAsVst3::getTailSamples()
{
	// options would be kNoTail, number of samples or kInfiniteTail
	// TODO: check how CLAP is handling this
	return Vst::kInfiniteTail;
}

IPlugView* PLUGIN_API ClapAsVst3::createView(FIDString name)
{
	if (_plugin->_ext._gui)
	{
		_wrappedview = new WrappedView(_plugin->_plugin, _plugin->_ext._gui,
			[&] {_wrappedview = nullptr; });
		return _wrappedview;
	}
	return nullptr;
}

//-----------------------------------------------------------------------------

tresult PLUGIN_API ClapAsVst3::getMidiControllerAssignment(int32 busIndex, int16 channel,
	Vst::CtrlNumber midiControllerNumber, Vst::ParamID& id/*out*/)
{
	// for my first Event bus and for MIDI channel 0 and for MIDI CC Volume only
	if (busIndex == 0 && channel == 0 ) // && midiControllerNumber == Vst::kCtrlVolume)
	{
		id = _IMidiMappingIDs[midiControllerNumber];
		return kResultTrue;
	}
	return kResultFalse;
}


////-----------------------------------------------------------------------------
//tresult PLUGIN_API ClapAsVst3::queryInterface(const TUID iid, void** obj)
//{
//	  DEF_INTERFACE(IMidiMapping)
//		return SingleComponentEffect::queryInterface(iid, obj);
//}

static Vst::SpeakerArrangement speakerArrFromPortType(const char* port_type)
{
	static const std::pair<const char*, Vst::SpeakerArrangement> arrangementmap[] =
	{
		{CLAP_PORT_MONO, Vst::SpeakerArr::kMono},
		{CLAP_PORT_STEREO, Vst::SpeakerArr::kStereo},
		// {CLAP_PORT_AMBISONIC, Vst::SpeakerArr::kAmbi1stOrderACN} <- we need also CLAP_EXT_AMBISONIC
		// {CLAP_PORT_SURROUND, Vst::SpeakerArr::kStereoSurround}, // add when CLAP_EXT_SURROUND is not draft anymore
		// TODO: add more PortTypes to Speaker Arrangement
		{nullptr,Vst::SpeakerArr::kEmpty}
	};

	auto p = &arrangementmap[0];
	while (p->first)
	{
		if (!strcmp(port_type, p->first))
		{
			return p->second;
		}
		++p;
	}

	return Vst::SpeakerArr::kEmpty;
}

void ClapAsVst3::addAudioBusFrom(const clap_audio_port_info_t* info, bool is_input)
{
	auto spk = speakerArrFromPortType(info->port_type);
	auto bustype = (info->flags & CLAP_AUDIO_PORT_IS_MAIN) ? Vst::BusTypes::kMain : Vst::BusTypes::kAux;
	// bool supports64bit = (info->flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS);
	Steinberg::char16 name16[256];
	Steinberg::str8ToStr16(&name16[0], info->name, 256);
	if (is_input)
	{
		addAudioInput(name16, spk, bustype, Vst::BusInfo::kDefaultActive);
	}
	else
	{
		addAudioOutput(name16, spk, bustype, Vst::BusInfo::kDefaultActive);
	}
	

}

void ClapAsVst3::addMIDIBusFrom(const clap_note_port_info_t* info, bool is_input)
{
	if ((info->supported_dialects & CLAP_NOTE_DIALECT_MIDI) || (info->supported_dialects & CLAP_NOTE_DIALECT_CLAP) )
	{
		Steinberg::char16 name16[256];
		Steinberg::str8ToStr16(&name16[0], info->name, 256);
		if (is_input)
		{
			addEventInput(name16, 1, Vst::BusTypes::kMain, Vst::BusInfo::kDefaultActive);
		}

	}
}

// Clap::IHost

void ClapAsVst3::setupAudioBusses(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* audioports)
{
	if (!audioports) return;
	auto numAudioInputs = audioports->count(plugin, true);
	auto numAudioOutputs = audioports->count(plugin, false);
	
	fprintf(stderr, "\tAUDIO in: %d, out: %d\n", (int)numAudioInputs, (int)numAudioOutputs);

  std::vector<clap_audio_port_info_t> inputs;
  std::vector<clap_audio_port_info_t> outputs;

  inputs.resize(numAudioInputs);
  outputs.resize(numAudioOutputs);

  for (decltype(numAudioInputs) i = 0; i < numAudioInputs; ++i)
  {
    clap_audio_port_info_t info;
    if (audioports->get(plugin, i, true, &info))
    {
      addAudioBusFrom(&info, true);
    }
  }
  for (decltype(numAudioOutputs) i = 0; i < numAudioOutputs; ++i)
  {
    clap_audio_port_info_t info;
    if (audioports->get(plugin, i, false, &info))
    {
      addAudioBusFrom(&info, false);
    }
  }
}

void ClapAsVst3::setupMIDIBusses(const clap_plugin_t* plugin, const clap_plugin_note_ports_t* noteports)
{
	if (!noteports) return;
	auto numMIDIInPorts = noteports->count(plugin, true);
	auto numMIDIOutPorts = noteports->count(plugin, false);

	// fprintf(stderr, "\tMIDI in: %d, out: %d\n", (int)numMIDIInPorts, (int)numMIDIOutPorts);

	std::vector<clap_note_port_info_t> inputs;
	std::vector<clap_note_port_info_t> outputs;

	inputs.resize(numMIDIInPorts);
	outputs.resize(numMIDIOutPorts);

	for (decltype(numMIDIInPorts) i = 0; i < numMIDIInPorts; ++i)
	{
		clap_note_port_info_t info;
		if (noteports->get(plugin, i, true, &info))
		{
			addMIDIBusFrom(&info, true);
		}
	}
	for (decltype(numMIDIOutPorts) i = 0; i < numMIDIOutPorts; ++i)
	{
		clap_note_port_info_t info;
		if (noteports->get(plugin, i, false, &info))
		{
			addMIDIBusFrom(&info, false);
		}
	}
}

void ClapAsVst3::setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params)
{
	if (!params) return;
	auto numparams = params->count(plugin);
	parameters.removeAll();
	this->parameters.init(numparams);
	for (decltype(numparams) i = 0; i < numparams; ++i)
	{
		clap_param_info info;
		if (params->get_info(plugin, i, &info))
		{
			auto p = Vst3Parameter::create(&info);
			parameters.addParameter(p);
		}
	}
	// find free tags for IMidiMapping
	Vst::ParamID x = 0xb00000;
	_IMidiMappingEasy = true;
	for (int i = 0; i < Vst::ControllerNumbers::kCountCtrlNumber; ++i)
	{
		while (parameters.getParameter(x))
		{
			// if this happens there is a index clash between the parameter ids
			// and the ones reserved for the IMidiMapping
			_IMidiMappingEasy = false;
			x++;
		}
		auto p = Vst3Parameter::create(0, i, x);
		parameters.addParameter(p);
		_IMidiMappingIDs[i] = x++;
	}
}


void ClapAsVst3::param_rescan(clap_param_rescan_flags flags) 
{
	auto vstflags = 0u;
	if (flags & CLAP_PARAM_RESCAN_ALL)
	{
		setupParameters(_plugin->_plugin,_plugin->_ext._params);
		vstflags |= Vst::RestartFlags::kMidiCCAssignmentChanged;
	}

	vstflags |= ((flags & CLAP_PARAM_RESCAN_VALUES) ? Vst::RestartFlags::kParamValuesChanged : 0u);
	vstflags |= ((flags & CLAP_PARAM_RESCAN_INFO) ? Vst::RestartFlags::kParamValuesChanged|Vst::RestartFlags::kParamTitlesChanged : 0u);
	if (vstflags != 0)
	{
		// update parameter values in our own tree
		auto len = parameters.getParameterCount();
		for (decltype(len) i = 0; i < len; ++i)
		{
			auto p = parameters.getParameterByIndex(i);
			auto p1 = static_cast<Vst3Parameter*>(p);
			double val;
			if (_plugin->_ext._params->get_value(_plugin->_plugin, p1->id, &val))
			{
				auto newval = p1->asVst3Value(val);
				if (p1->getNormalized() != newval)
				{
					p1->setNormalized(newval);
				}
			}
		}
		this->componentHandler->restartComponent(vstflags);
	}

}

void ClapAsVst3::param_clear(clap_id param, clap_param_clear_flags flags)
{	
}

void ClapAsVst3::param_request_flush()
{
	if (!this->_processing)
	{
		// _plugin->_ext._params->flush(_plugin->_plugin, in, out);
	}
}

bool ClapAsVst3::gui_can_resize()
{
	// the plugin asks if the host can do a resize
	return (componentHandler2 != nullptr);
}

bool ClapAsVst3::gui_request_resize(uint32_t width, uint32_t height)
{
	return _wrappedview->request_resize(width, height);
}

bool ClapAsVst3::gui_request_show()
{
	if ( componentHandler2)
		return (componentHandler2->requestOpenEditor() == kResultOk);
	return false;
}

bool ClapAsVst3::gui_request_hide()
{
	return false;
}


void ClapAsVst3::mark_dirty()
{
	if ( componentHandler2)
		componentHandler2->setDirty(true);
}

void ClapAsVst3::schnick()
{
	// OutputDebugString("Schnick!");
}

void ClapAsVst3::onBeginEdit(clap_id id) 
{
	// receive beginEdit and pass it to the internal queue
	_queueToUI.push( beginEvent(id) );
	
}
void ClapAsVst3::onPerformEdit(const clap_event_param_value_t* value)
{
	// receive a value change and pass it to the internal queue
	_queueToUI.push(valueEvent(value));
}
void ClapAsVst3::onEndEdit(clap_id id) 
{
	_queueToUI.push(endEvent(id));

}

bool ClapAsVst3::register_timer(uint32_t period_ms, clap_id* timer_id)
{	
	// restricting the callbacks to ~30 Hz
	if (period_ms < 30)
	{
		period_ms = 30;
	}
	auto l = _timersObjects.size();
	for ( decltype(l) i = 0 ; i < l ; ++i)
	{
		auto& to = _timersObjects[i];
		if (to.period == 0)
		{
			// reuse timer object
			to.timer_id = i;
			to.period = period_ms;
			to.nexttick = os::getTickInMS() + period_ms;
			// pass the id to the plugin
			*timer_id = to.timer_id;
			return true;
		}
	}
	// create a new timer object
	auto newid = (clap_id)l;
	TimerObject f{ period_ms, os::getTickInMS() + period_ms, newid };
	*timer_id = newid;
	_timersObjects.push_back(f);
	return true;
}
bool ClapAsVst3::unregister_timer(clap_id timer_id)
{
	for (auto& to : _timersObjects)
	{
		if (to.timer_id == timer_id)
		{
			to.period = 0;
			to.nexttick = 0;
			return true;
		}
	}
	return false;
}

void ClapAsVst3::onIdle()
{
	// handling queued events
	queueEvent n;
	while (_queueToUI.pop(n))
	{
		switch (n._type)
		{
		case queueEvent::type_t::editstart:
			beginEdit(n._data._id);
			break;
		case queueEvent::type_t::editvalue:
		{
			auto param = (Vst3Parameter*)(parameters.getParameter(n._data._value.param_id));
			auto v = n._data._value.value;
			performEdit(n._data._value.param_id, param->asVst3Value(v));
		}
			break;
		case queueEvent::type_t::editend:
			endEdit(n._data._id);
			break;
		}
	}

	// handling timerobjects
	auto now = os::getTickInMS();
	for (auto&& to : _timersObjects)
	{
		if (to.period > 0)
		{
			if (to.nexttick < now)
			{
				to.nexttick = now + to.period;
				this->_plugin->_ext._timer->on_timer(_plugin->_plugin, to.timer_id);
			}
		}
	}
}
