#include "generated_entrypoints.hxx"
#include "detail/auv2/process.h"
#include <set>

extern bool fillAudioUnitCocoaView(AudioUnitCocoaViewInfo* viewInfo, std::shared_ptr<Clap::Plugin>);

namespace free_audio::auv2_wrapper
{

Clap::Library _library;  // holds the library with plugins

#if 0
--- 8< ---
struct ClapHostExtensions
{
  static inline WrapAsAUV2* self(const clap_host_t* host)
  {
    return static_cast<WrapAsAUV2*>(host->host_data);
  }
  static void mark_dirty(const clap_host_t* host)
  {
    self(host)->mark_dirty();
  }
  const clap_host_state_t _state = {mark_dirty};
};
#endif

void pffffrzz()
{
  auto pid = getpid();

  char buf[200];
  snprintf(buf, sizeof(buf), "process %d", pid);
  SInt32 nRes = 0;
  CFUserNotificationRef pDlg = NULL;
  CFStringRef b = CFStringCreateWithCString(NULL, buf, kCFStringEncodingASCII);
  const void* keys[] = {kCFUserNotificationAlertHeaderKey, kCFUserNotificationAlertMessageKey};
  const void* vals[] = {CFSTR("Test Foundation Message Box"), b};

  CFDictionaryRef dict =
      CFDictionaryCreate(0, keys, vals, sizeof(keys) / sizeof(*keys), &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);

  pDlg =
      CFUserNotificationCreate(kCFAllocatorDefault, 0, kCFUserNotificationPlainAlertLevel, &nRes, dict);

  (void)pDlg;
  CFRelease(b);
  usleep(20000000);
}

bool WrapAsAUV2::initializeClapDesc()
{
  LOGINFO("[clap-wrapper] auv2: id={} index: {}", _clapid, _idx);

  if (!_library.hasEntryPoint())
  {
    if (_clapname.empty())
    {
      std::cout << "[ERROR] _clapname (" << _clapname << ") empty and no internal entry point"
                << std::endl;
    }

    auto csp = Clap::getValidCLAPSearchPaths();
    auto it = std::find_if(csp.begin(), csp.end(),
                           [this](const auto& cs)
                           {
                             auto fp = cs / (_clapname + ".clap");
                             return fs::is_directory(fp) && _library.load(fp);
                           });

    if (it != csp.end())
    {
      std::cout << "[clap-wrapper] auv2 loaded clap from " << it->u8string() << std::endl;
    }
    else
    {
      std::cout << "[ERROR] cannot load clap" << std::endl;
      return false;
    }
  }

  if (_clapid.empty())
  {
    if (_idx < 0 || _idx >= (int)_library.plugins.size())
    {
      std::cout << "[ERROR] cannot load by index" << std::endl;
      return false;
    }
    _desc = _library.plugins[_idx];
  }
  else
  {
    for (auto* d : _library.plugins)
    {
      if (strcmp(d->id, _clapid.c_str()) == 0)
      {
        _desc = d;
      }
    }
  }

  if (!_desc)
  {
    std::cout << "[ERROR] cannot determine plugin description" << std::endl;
    return false;
  }
  return true;
}

WrapAsAUV2::WrapAsAUV2(AUV2_Type type, const std::string& clapname, const std::string& clapid, int idx,
                       AudioComponentInstance ci)
  : Base{ci, 0, 0}  // these elements are set correctly in ::PostConstructor
  , Clap::IHost()
  , Clap::IAutomation()
  , os::IPlugObject()
  , _autype(type)
  , _clapname{clapname}
  , _clapid{clapid}
  , _idx{idx}
  , _os_attached([this] { os::attach(this); }, [this] { os::detach(this); })
{
  (void)_autype;  // TODO: will be used for dynamic property adaption
  _uiIsOpened = false;
  if (!_desc)
  {
    if (initializeClapDesc())
    {
      std::cout << "[clap-wrapper] auv2: Initialized '" << _desc->id << "' / '" << _desc->name << "' / '"
                << _desc->version << "'" << std::endl;
      /*
       * ToDo: Stand up the host, create the plugin instance here
       */

      // pffffrzz();  // <- enable this to have a hook to attach a debugger
      _plugin = Clap::Plugin::createInstance(_library._pluginFactory, _desc->id, this);

      _plugin->initialize();
      _os_attached.on();
    }
  }
}

WrapAsAUV2::~WrapAsAUV2()
{
  if (_plugin)
  {
    if (_uiIsOpened && _uiconn._canary)
    {
      *_uiconn._canary = 0;       // notify the view
      _uiconn._canary = nullptr;  // disable the canary reference

      // close destroy the gui ourselves
      _plugin->_ext._gui->destroy(_plugin->_plugin);
      _uiIsOpened = false;
    }

    _os_attached.off();
    _plugin->terminate();
    _plugin.reset();
  }
  if (_current_program_name)
  {
    CFRelease(_current_program_name);
  }
}

// the very very reduced state machine
// this is where AUv2 transitions to render-ready
OSStatus WrapAsAUV2::Initialize()
{
  if (!_desc) return 2;

  // first need to initialize the base to create
  // all elements needed
  auto res = Base::Initialize();
  if (res != noErr) return res;

  // activating the plugin in AU can happen in the Audio Thread (Logic Pro)
  // CLAP does not want it, therefore the wrapper insists on being in the
  // main thread
  auto guarantee_mainthread = _plugin->AlwaysMainThread();
  activateCLAP();

#if 0
  // get our current numChannels for input and output
  const auto auNumInputs = static_cast<SInt16>(Input(0).GetStreamFormat().mChannelsPerFrame);
  const auto auNumOutputs = static_cast<SInt16>(Output(0).GetStreamFormat().mChannelsPerFrame);

  // does the unit publish specific information about channel configurations?
  const AUChannelInfo* auChannelConfigs = nullptr;
  const UInt32 numIOconfigs = SupportedNumChannels(&auChannelConfigs);

  if ((numIOconfigs > 0) && (auChannelConfigs != nullptr)) {
    bool foundMatch = false;
    for (UInt32 i = 0; (i < numIOconfigs) && !foundMatch; ++i) {
      const SInt16 configNumInputs = auChannelConfigs[i].inChannels;   // NOLINT
      const SInt16 configNumOutputs = auChannelConfigs[i].outChannels; // NOLINT
      if ((configNumInputs < 0) && (configNumOutputs < 0)) {
        // unit accepts any number of channels on input and output
        if (((configNumInputs == -1) && (configNumOutputs == -2)) ||
          ((configNumInputs == -2) &&
            (configNumOutputs == -1))) { // NOLINT repeated branch below
          foundMatch = true;
          // unit accepts any number of channels on input and output IFF they are the same
          // number on both scopes
        } else if (((configNumInputs == -1) && (configNumOutputs == -1)) &&
               (auNumInputs == auNumOutputs)) {
          foundMatch = true;
          // unit has specified a particular number of channels on both scopes
        } else {
          continue;
        }
      } else {
        // the -1 case on either scope is saying that the unit doesn't care about the
        // number of channels on that scope
        const bool inputMatch = (auNumInputs == configNumInputs) || (configNumInputs == -1);
        const bool outputMatch =
          (auNumOutputs == configNumOutputs) || (configNumOutputs == -1);
        if (inputMatch && outputMatch) {
          foundMatch = true;
        }
      }
    }
    if (!foundMatch) {
      return kAudioUnitErr_FormatNotSupported;
    }
  } else {
    // there is no specifically published channel info
    // so for those kinds of effects, the assumption is that the channels (whatever their
    // number) should match on both scopes
    if ((auNumOutputs != auNumInputs) || (auNumOutputs == 0)) {
      return kAudioUnitErr_FormatNotSupported;
    }
  }
#endif
#if 0
  MaintainKernels();

  mMainOutput = &Output(0);
  mMainInput = &Input(0);

  const AudioStreamBasicDescription format = GetStreamFormat(kAudioUnitScope_Output, 0);
  mBytesPerFrame = format.mBytesPerFrame;

  return noErr;
#endif

  return noErr;
}

void WrapAsAUV2::setupWrapperSpecifics(const clap_plugin_t* plugin)
{
  // TODO: if there are AUv2 specific extensions, they can be retrieved here
  // _auv2_specifics = (clap_plugin_as_auv2_t*)plugin->get_extension(plugin, CLAP_PLUGIN_AS_AUV2);
}

void WrapAsAUV2::setupAudioBusses(const clap_plugin_t* plugin,
                                  const clap_plugin_audio_ports_t* audioports)
{
  auto numAudioInputs = audioports->count(plugin, true);
  auto numAudioOutputs = audioports->count(plugin, false);

  LOGINFO("[clap-wrapper] Setup Busses: audio in: {}, out: {}", (int)numAudioInputs,
          (int)numAudioOutputs);

  ausdk::AUBase::GetScope(kAudioUnitScope_Input).Initialize(this, kAudioUnitScope_Input, numAudioInputs);

  for (decltype(numAudioInputs) i = 0; i < numAudioInputs; ++i)
  {
    clap_audio_port_info_t info;
    if (audioports->get(plugin, i, true, &info))
    {
      addAudioBusFrom(i, &info, true);
    }
  }

  ausdk::AUBase::GetScope(kAudioUnitScope_Output)
      .Initialize(this, kAudioUnitScope_Output, numAudioOutputs);

  for (decltype(numAudioOutputs) i = 0; i < numAudioOutputs; ++i)
  {
    clap_audio_port_info_t info;
    if (audioports->get(plugin, i, false, &info))
    {
      addAudioBusFrom(i, &info, false);
    }
  }

  ausdk::AUBase::ReallocateBuffers();

}  // called from initialize() to allow the setup of audio ports

void WrapAsAUV2::setupMIDIBusses(const clap_plugin_t* plugin, const clap_plugin_note_ports_t* noteports)
{
  // TODO: figure out if MIDI is is preferred as CLAP or Notes
  if (!noteports) return;
  auto numMIDIInPorts = noteports->count(plugin, true);
  auto numMIDIOutPorts = noteports->count(plugin, false);
  (void)numMIDIOutPorts;  // TODO: remove this when MIDI out is implemented

  // fprintf(stderr, "\tMIDI in: %d, out: %d\n", (int)numMIDIInPorts, (int)numMIDIOutPorts);
  /*
  std::vector<clap_note_port_info_t> inputs;
  std::vector<clap_note_port_info_t> outputs;

  inputs.resize(numMIDIInPorts);
  outputs.resize(numMIDIOutPorts);
*/
  _midi_wants_midi_input = (numMIDIInPorts > 0);
  // in AU we don't have different MIDI INs, therefore we just use one
  if (numMIDIInPorts > 0)
  {
    clap_note_port_info_t info;
    if (noteports->get(plugin, 0, true, &info))
    {
      _midi_preferred_dialect = info.preferred_dialect;
      _midi_understands_midi2 = (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI2);
    }
  }
  if (numMIDIOutPorts > 0)
  {
    for (decltype(numMIDIOutPorts) i = 0; i < numMIDIOutPorts; ++i)
    {
      clap_note_port_info_t info;
      if (noteports->get(plugin, i, false, &info))
      {
        _midi_outports.emplace_back(std::make_unique<MIDIOutput>(_midi_outports.size(), info));
      }
    }
  }
}

void WrapAsAUV2::setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params)
{
  auto guarantee_mainthread = _plugin->AlwaysMainThread();
  // creating parameters.

  _clumps.reset();
  auto* p = _plugin->_ext._params;
  if (p)
  {
    uint32_t numparams = p->count(_plugin->_plugin);
    clap_param_info_t paraminfo;
    for (uint32_t i = 0; i < numparams; ++i)
    {
      if (p->get_info(_plugin->_plugin, i, &paraminfo))
      {
        double result;
        if (p->get_value(_plugin->_plugin, paraminfo.id, &result))
        {
          // If the parametre is already created, just restate its info
          auto piter = _parametertree.find(paraminfo.id);
          if (piter == _parametertree.end())
          {
            // creating the mapping object and insert it into the tree
            // this will also create Clumps if necessary
            _parametertree[paraminfo.id] = std::make_unique<Clap::AUv2::Parameter>(paraminfo);
          }
          else
          {
            piter->second->resetInfo(paraminfo);
          }
          Globals()->SetParameter(paraminfo.id, result);
        }
      }
    }
  }
}

OSStatus WrapAsAUV2::GetParameterList(AudioUnitScope inScope, AudioUnitParameterID* outParameterList,
                                      UInt32& outNumParameters)
{
  return AUBase::GetParameterList(inScope, outParameterList, outNumParameters);
}

void WrapAsAUV2::param_rescan(clap_param_rescan_flags flags)
{
  // Re-call setup parameters which will just reset info if the param exists
  setupParameters(_plugin->_plugin, _plugin->_ext._params);

  // if ( flags & CLAP_PARAM_RESCAN_ALL) // TODO: check out how differentiated we can do this
  {
    PropertyChanged(kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0);
    PropertyChanged(kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, 0);
    PropertyChanged(kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0);
    return;
  }

  // This code doesn't actually do what we want but leave it hear for the comment
  // above and future investigation
#if 0
  AudioUnitEvent myEvent;
  myEvent.mArgument.mProperty.mAudioUnit = GetComponentInstance();
  myEvent.mArgument.mProperty.mScope = kAudioUnitScope_Global;
  myEvent.mArgument.mProperty.mElement = 0;
  myEvent.mEventType = kAudioUnitEvent_PropertyChange;

  {
    for (auto& i : _parametertree)
    {
      *of << "Considering param" << std::endl;
      if (i.second->info().flags & CLAP_PARAM_IS_AUTOMATABLE)
      {
        *of << "which is automatable" << std::endl;
        myEvent.mArgument.mProperty.mElement = i.second->info().id;

        if (flags & CLAP_PARAM_RESCAN_INFO)
        {
          *of << "rescan info" << std::endl;
          myEvent.mArgument.mProperty.mPropertyID = kAudioUnitProperty_ParameterInfo;
          AUEventListenerNotify(NULL, NULL, &myEvent);
          myEvent.mArgument.mProperty.mPropertyID = kAudioUnitProperty_ParameterIDName;
          AUEventListenerNotify(NULL, NULL, &myEvent);
        }

        if (flags & CLAP_PARAM_RESCAN_TEXT)
        {
          *of << "wrescan text" << std::endl;
          myEvent.mArgument.mProperty.mPropertyID = kAudioUnitProperty_ParameterValueStrings;
          AUEventListenerNotify(NULL, NULL, &myEvent);
        }
      }
    }
  }
#endif
}

// outParameterList may be a null pointer
OSStatus WrapAsAUV2::GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID,
                                      AudioUnitParameterInfo& outParameterInfo)
{
  // const uint64_t stdflag = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
  if (inScope == kAudioUnitScope_Global)
  {
    AudioUnitParameterOptions flags = 0;
    auto pi = _parametertree.find(inParameterID);
    if (pi != _parametertree.end())
    {
      auto f = pi->second.get();
      const auto info = f->info();

      flags |= kAudioUnitParameterFlag_Global;

      if (!(info.flags & CLAP_PARAM_IS_AUTOMATABLE)) flags |= kAudioUnitParameterFlag_NonRealTime;
      if (!(info.flags & CLAP_PARAM_IS_HIDDEN))
      {
        if (info.flags & CLAP_PARAM_IS_READONLY)
          flags |= kAudioUnitParameterFlag_IsReadable;
        else
          flags |= kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
      }
      if (info.flags & CLAP_PARAM_IS_STEPPED)
      {
        if (info.max_value - info.min_value == 1)
        {
          flags |= kAudioUnitParameterUnit_Boolean;
        }
        // flags |= kAudioUnitParameterUnit_Indexed;  // probably need to add the lists then
      }
      if (info.max_value - info.min_value > 100)
      {
        flags |= kAudioUnitParameterFlag_IsHighResolution;
      }

      // checking if the parameter supports the conversion of its value to text

      // we can't get the value since we are not in the audio thread
      auto guarantee_mainthread = _plugin->AlwaysMainThread();
      double value;
      if (_plugin->_ext._params->get_value(_plugin->_plugin, info.id, &value))
      {
        char buf[200];
        if (_plugin->_ext._params->value_to_text(_plugin->_plugin, info.id, value, buf, sizeof(buf)))
        {
          flags |= kAudioUnitParameterFlag_HasName;
        }
      }

      /*
       * The CFString() used from the param can reset which releases it. So add a ref count
       * and ask the param to release it too
       */
      flags |= kAudioUnitParameterFlag_HasCFNameString | kAudioUnitParameterFlag_CFNameRelease;

      outParameterInfo.flags = flags;

      // according to the documentation, the name field should be zeroed. In fact, AULab does display anything then.
      // strcpy(outParameterInfo.name, info.name);
      memset(outParameterInfo.name, 0, sizeof(outParameterInfo.name));

      CFRetain(f->CFString());
      outParameterInfo.cfNameString = f->CFString();
      outParameterInfo.minValue = info.min_value;
      outParameterInfo.maxValue = info.max_value;
      outParameterInfo.defaultValue = info.min_value;
      if (info.min_value < 0.0)
      {
        outParameterInfo.defaultValue = 0.0;
      }

      // adding the clump information
      if (info.module[0] != 0)
      {
        outParameterInfo.flags |= kAudioUnitParameterFlag_HasClump;
        outParameterInfo.clumpID = _clumps.addClump(info.module);
      }
      return noErr;
    }
  }
  return AUBase::GetParameterInfo(inScope, inParameterID, outParameterInfo);
}

OSStatus WrapAsAUV2::SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope,
                                  AudioUnitElement inElement, AudioUnitParameterValue inValue,
                                  UInt32 inBufferOffsetInFrames)
{
  if (inScope == kAudioUnitScope_Global)
  {
    if (_processAdapter)
    {
      // a parameter has been set.
      // _processAdapter->addParameterEvent(inID,inValue,inBufferOffsetInFrames);
      auto p = _parametertree.find(inID);
      if (p != _parametertree.end())
      {
        auto& param = p->second.get()->info();
        _processAdapter->addParameterEvent(param, inValue, inBufferOffsetInFrames);
      }
    }
  }
  return AUBase::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);
}

OSStatus WrapAsAUV2::CopyClumpName(AudioUnitScope inScope, UInt32 inClumpID, UInt32 inDesiredNameLength,
                                   CFStringRef* outClumpName)
{
  if (inScope == kAudioUnitScope_Global)
  {
    auto p = _clumps.getClump(inClumpID);
    if (p)
    {
      *outClumpName = CFStringCreateWithCString(NULL, p, kCFStringEncodingUTF8);
      return noErr;
    }
  }
  return kAudioUnitErr_InvalidProperty;
}

OSStatus WrapAsAUV2::Start()
{
  // _plugin->start_processing();
  // activateCLAP();
  return noErr;  // Base::Start();
}

OSStatus WrapAsAUV2::Stop()
{
  // _plugin->stop_processing();
  // deactivateCLAP();
  return noErr;  // Base::Stop();
}
void WrapAsAUV2::Cleanup()
{
  LOGINFO("[clap-wrapper] Cleaning up Plugin");
  auto guarantee_mainthread = _plugin->AlwaysMainThread();
  if (this->_uiIsOpened)
  {
    LOGINFO("[clap-wrapper] !! UI still open, destroying UI and disconnecting view");
    if (_uiconn._canary)
    {
      *_uiconn._canary = 0;       // reset the canary
      _uiconn._canary = nullptr;  // and disconnect it
      if (_plugin->_plugin && _plugin->_ext._gui)
      {
        this->_uiconn._destroyWindow();
        this->_plugin->_ext._gui->destroy(_plugin->_plugin);
      }
    }
  }
  deactivateCLAP();
  Base::Cleanup();
}

Float64 WrapAsAUV2::GetLatency()
{
  if (_plugin && _plugin->_ext._latency)
  {
    auto samplerate = this->GetStreamFormat(kAudioUnitScope_Output, 0).mSampleRate;
    auto latency_in_samples = (double)(_plugin->_ext._latency->get(_plugin->_plugin));
    Float64 latencytime = latency_in_samples / samplerate;

    return latencytime;
  }
  return 0.0;
}

Float64 WrapAsAUV2::GetTailTime()
{
  if (_plugin && _plugin->_ext._tail)
  {
    auto samplerate = this->GetStreamFormat(kAudioUnitScope_Output, 0).mSampleRate;
    auto tailtime_in_samples = (double)(_plugin->_ext._tail->get(_plugin->_plugin));
    Float64 tailtime = tailtime_in_samples / samplerate;

    return tailtime;
  }
  return 0.0;
}

OSStatus WrapAsAUV2::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                     AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable)
{
  if (inScope == kAudioUnitScope_Global)
  {
    switch (inID)
    {
      case kAudioUnitProperty_ParameterStringFromValue:
        if (inScope == kAudioUnitScope_Global)
        {
          outDataSize = sizeof(AudioUnitParameterStringFromValue);
          outWritable = true;
          return noErr;
        }
        break;
      case kMusicDeviceProperty_InstrumentCount:
        outDataSize = sizeof(UInt32);
        outWritable = false;
        return noErr;
        break;
      case kAudioUnitProperty_BypassEffect:
        // case kAudioUnitProperty_InPlaceProcessing:
        outWritable = true;
        outDataSize = sizeof(UInt32);
        return noErr;
#ifdef DUAL_SCHEDULING_ENABLED
      case kMusicDeviceProperty_DualSchedulingMode:
        outWritable = true;
        outDataSize = sizeof(UInt32);
        return noErr;
        break;
#endif
      case kMusicDeviceProperty_SupportsStartStopNote:
        outWritable = true;
        outDataSize = sizeof(UInt32);
        return noErr;
        break;

      case kAudioUnitProperty_CocoaUI:
        if (!_plugin->_ext._gui) return kAudioUnitErr_InvalidProperty;
        if (!_plugin->_ext._gui->is_api_supported(_plugin->_plugin, CLAP_WINDOW_API_COCOA, false))
          return kAudioUnitErr_InvalidProperty;
        outWritable = false;
        outDataSize = sizeof(struct AudioUnitCocoaViewInfo);
        return noErr;
        break;

      case kAudioUnitProperty_MIDIOutputCallbackInfo:
        outDataSize = sizeof(CFArrayRef);
        outWritable = false;
        return noErr;
        break;
      case kAudioUnitProperty_MIDIOutputCallback:
        outWritable = true;
        outDataSize = sizeof(AUMIDIOutputCallbackStruct);
        break;

        // custom
      case kAudioUnitProperty_ClapWrapper_UIConnection_id:
        outWritable = false;
        outDataSize = sizeof(free_audio::auv2_wrapper::ui_connection);
        return noErr;
        break;
      default:
        break;
    }
  }
  return Base::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
}

OSStatus WrapAsAUV2::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                 AudioUnitElement inElement, void* outData)
{
  if (inScope == kAudioUnitScope_Global)
  {
    switch (inID)
    {
      case kAudioUnitProperty_ParameterStringFromValue:
      {
        //         _plugin->_ext._params->value_to_text(
        auto guarantee_mainthread = _plugin->AlwaysMainThread();

        char buf[200];
        auto p = (AudioUnitParameterStringFromValue*)(outData);
        double value = *p->inValue;
        if (_plugin->_ext._params->value_to_text(_plugin->_plugin, p->inParamID, value, buf, 200))
        {
          p->outString = CFStringCreateWithCString(NULL, buf, kCFStringEncodingUTF8);
          return noErr;
        }
        return kAudioUnitErr_InvalidProperty;
      }
      break;
      case kMusicDeviceProperty_InstrumentCount:
        if (inScope != kAudioUnitScope_Global)
        {
          return kAudioUnitErr_InvalidScope;
        }
        // For a MusicDevice that doesn't support separate instruments (ie. is mono-timbral)
        // then this call should return an instrument count of zero and noErr
        *static_cast<UInt32*>(outData) = 0;
        if (_autype == AUV2_Type::aumu_musicdevice) return noErr;
        return kAudioUnitErr_InvalidProperty;
        // return  GetInstrumentCount(*static_cast<UInt32*>(outData));

      case kAudioUnitProperty_BypassEffect:
        *static_cast<UInt32*>(outData) = (IsBypassEffect() ? 1 : 0);  // NOLINT
        return noErr;
        //    case kAudioUnitProperty_InPlaceProcessing:
        //      *static_cast<UInt32*>(outData) = (mProcessesInPlace ? 1 : 0); // NOLINT
        //      return noErr;
      case kAudioUnitProperty_ClapWrapper_UIConnection_id:
        _uiconn._plugin = _plugin.get();
        _uiconn._window = nullptr;
        _uiconn._registerWindow = [this](auto* x, auto* y)
        {
          this->_uiconn._window = x;
          this->_uiconn._canary = y;
        };
        _uiconn._createWindow = [this]
        {
          this->_uiIsOpened = true;
          _plugin->_ext._gui->create(_plugin->_plugin, CLAP_WINDOW_API_COCOA, false);
        };
        _uiconn._destroyWindow = [this]
        {
          // this must exist
          _plugin->_ext._gui->destroy(_plugin->_plugin);

          this->_uiIsOpened = false;
          if (this->_uiconn._canary)
          {
            *(this->_uiconn._canary) = 0;
          }
        };
        *static_cast<ui_connection*>(outData) = _uiconn;
        return noErr;

      case kAudioUnitProperty_CocoaUI:
        LOGINFO("[clap-wrapper] Property: kAudioUnitProperty_CocoaUI {}",
                (_plugin) ? "plugin" : "no plugin");
        if (_plugin && _plugin->_ext._gui &&
            (_plugin->_ext._gui->is_api_supported(_plugin->_plugin, CLAP_WINDOW_API_COCOA, false)))
        {
          fillAudioUnitCocoaView(((AudioUnitCocoaViewInfo*)outData), _plugin);
          LOGINFO("[clap-wrapper] kAudioUnitProperty_CocoaUI complete");
          return noErr;  // sizeof(AudioUnitCocoaViewInfo);
        }
        else
        {
          LOGINFO("[clap-wrapper] Mysterious: kAudioUnitProperty_CocoaUI although now plugin ext");
          fillAudioUnitCocoaView(((AudioUnitCocoaViewInfo*)outData), _plugin);
          return noErr;
        }
        return kAudioUnitErr_InvalidProperty;
        break;
      case kAudioUnitProperty_MIDIOutputCallbackInfo:
        if (_midi_outports.size() > 0)
        {
          CFMutableArrayRef callbackArray =
              CFArrayCreateMutable(NULL, _midi_outports.size(), &kCFTypeArrayCallBacks);

          for (const auto& portinfo : _midi_outports)
          {
            CFStringRef str =
                CFStringCreateWithCString(NULL, portinfo->_info.name, kCFStringEncodingUTF8);
            CFArrayAppendValue(callbackArray, str);
          }

          CFArrayRef array = CFArrayCreateCopy(NULL, callbackArray);

          *(CFArrayRef*)outData = array;

          CFRelease(callbackArray);

          return noErr;
        }
        return kAudioUnitErr_InvalidProperty;
        break;
#ifdef DUAL_SCHEDULING_ENABLED
      case kMusicDeviceProperty_DualSchedulingMode:
        // yes we do
        // *static_cast<UInt32*>(outData) = 1;
        return noErr;
        break;
#endif
      case kMusicDeviceProperty_SupportsStartStopNote:
        // TODO: change this when figured out how the NoteParamsControlValue actually do work.
        *static_cast<UInt32*>(outData) = 1;
        return noErr;
        break;
      default:
        break;
    }
  }
  return Base::GetProperty(inID, inScope, inElement, outData);
}
OSStatus WrapAsAUV2::SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                 AudioUnitElement inElement, const void* inData, UInt32 inDataSize)
{
  if (inScope == kAudioUnitScope_Global)
  {
    switch (inID)
    {
      case kAudioUnitProperty_BypassEffect:
      {
        if (inDataSize < sizeof(UInt32))
        {
          return kAudioUnitErr_InvalidPropertyValue;
        }

        const bool tempNewSetting = *static_cast<const UInt32*>(inData) != 0;
        // we're changing the state of bypass
        if (tempNewSetting != IsBypassEffect())
        {
          if (!tempNewSetting && IsBypassEffect() && IsInitialized())
          {  // turning bypass off and we're initialized
            Reset(kAudioUnitScope_Global, 0);
          }
          SetBypassEffect(tempNewSetting);
        }
        return noErr;
      }
      //    case kAudioUnitProperty_InPlaceProcessing:
      //      mProcessesInPlace = *static_cast<const UInt32*>(inData) != 0;
      //      return noErr;
      break;
      case kAudioUnitProperty_MIDIOutputCallbackInfo:
        // this is actually read only
        return noErr;
        break;
      case kAudioUnitProperty_MIDIOutputEventListCallback:
        break;
      case kAudioUnitProperty_MIDIOutputCallback:
        if (inDataSize < sizeof(AUMIDIOutputCallbackStruct)) return kAudioUnitErr_InvalidPropertyValue;

        if (inData)
        {
          _midioutput_hostcallback = *static_cast<const AUMIDIOutputCallbackStruct*>(inData);
        }
        else
        {
          _midioutput_hostcallback.midiOutputCallback = nullptr;
          _midioutput_hostcallback.userData = nullptr;
        }
        return noErr;
        break;

#ifdef DUAL_SCHEDULING_ENABLED

      case kMusicDeviceProperty_DualSchedulingMode:
      {
        auto x = *static_cast<const UInt32*>(inData);
        if (x > 0) LOGINFO("Host supports DualSchedulung Mode");
        _midi_dualscheduling_mode = (x != 0);
        return noErr;
      }
      break;
#endif
      case kMusicDeviceProperty_SupportsStartStopNote:
      {
        // TODO: we probably want to use start/stop note
        auto x = *static_cast<const UInt32*>(inData);
        (void)x;
        return noErr;
      }
      break;

      default:
        break;
    }
  }
  auto xxx = Base::SetProperty(inID, inScope, inElement, inData, inDataSize);
  if (xxx == kAudioUnitErr_InvalidElement)
  {
    ;
  }
  return xxx;
}

OSStatus WrapAsAUV2::SetRenderNotification(AURenderCallback inProc, void* inRefCon)
{
  // activateCLAP();

  return Base::SetRenderNotification(inProc, inRefCon);
}

OSStatus WrapAsAUV2::RemoveRenderNotification(AURenderCallback inProc, void* inRefCon)
{
  // deactivateCLAP();
  return Base::RemoveRenderNotification(inProc, inRefCon);
}

void WrapAsAUV2::latency_changed()
{
  PropertyChanged(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
}

void WrapAsAUV2::tail_changed()
{
  PropertyChanged(kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0);
}

void WrapAsAUV2::addAudioBusFrom(int bus, const clap_audio_port_info_t* info, bool is_input)
{
  // add/set audio bus configuration from info to appropriate scope
  LOGINFO("[clap-wrapper]     - add bus {} : {}", bus, is_input ? "In" : "Out");
  if (is_input)
  {
    addInputBus(bus, info);
  }
  else
  {
    addOutputBus(bus, info);
  }
}

void WrapAsAUV2::addInputBus(int bus, const clap_audio_port_info_t* info)
{
  auto& busref = Input(bus);

  CFStringRef busNameString = CFStringCreateWithCString(NULL, info->name, kCFStringEncodingUTF8);
  busref.SetName(busNameString);
  CFRelease(busNameString);

  auto sf = busref.GetStreamFormat();
  sf.mChannelsPerFrame = info->channel_count;
  busref.SetStreamFormat(sf);
}
void WrapAsAUV2::addOutputBus(int bus, const clap_audio_port_info_t* info)
{
  auto& busref = Output(bus);

  CFStringRef busNameString = CFStringCreateWithCString(NULL, info->name, kCFStringEncodingUTF8);
  busref.SetName(busNameString);
  CFRelease(busNameString);

  auto sf = busref.GetStreamFormat();
  sf.mChannelsPerFrame = info->channel_count;
  busref.SetStreamFormat(sf);
}

void WrapAsAUV2::activateCLAP()
{
  if (_plugin)
  {
    assert(!_initialized);
    if (!_processAdapter) _processAdapter = std::make_unique<Clap::AUv2::ProcessAdapter>();
    auto maxSampleFrames = Base::GetMaxFramesPerSlice();
    auto minSampleFrames = (maxSampleFrames >= 16) ? 16 : 1;
    _plugin->setBlockSizes(minSampleFrames, maxSampleFrames);
    _plugin->setSampleRate(Output(0).GetStreamFormat().mSampleRate);

    _processAdapter->setupProcessing(Inputs(), Outputs(), _plugin->_plugin, _plugin->_ext._params, this,
                                     &_parametertree, this, maxSampleFrames, _midi_preferred_dialect);

    _plugin->activate();
    _plugin->start_processing();
    _initialized = true;
  }
}

void WrapAsAUV2::deactivateCLAP()
{
  if (_plugin)
  {
    _initialized = false;
    _processAdapter.reset();
    _plugin->stop_processing();
    _plugin->deactivate();
  }
  _midioutput_hostcallback = {nullptr, nullptr};
}

OSStatus WrapAsAUV2::Render(AudioUnitRenderActionFlags& inFlags, const AudioTimeStamp& inTimeStamp,
                            UInt32 inFrames)
{
  assert(inFlags == 0);
  if (_initialized && (inFlags == 0))
  {
    // do the render dance
    Clap::AUv2::ProcessData data{inFlags, inTimeStamp, inFrames, this};

    // retrieve musical information for this render block

    auto hcb = GetHostCallbackInfo();
    if (hcb.transportStateProc2)
    {
      data._AUtransportValid =
          (noErr == (hcb.transportStateProc2)(hcb.hostUserData, &data._isPlaying, &data._isRecording,
                                              &data._transportChanged, &data._currentSongPosInSeconds,
                                              &data._isLooping, &data._cycleStart, &data._cycleEnd));
    }
    else
    {
      data._isRecording = FALSE;

      data._AUtransportValid =
          (noErr == CallHostTransportState(&data._isPlaying, &data._transportChanged,
                                           &data._currentSongPosInSeconds, &data._isLooping,
                                           &data._cycleStart, &data._cycleEnd));
    }

    data._currentSongPosInSeconds /= std::max(_plugin->getSampleRate(), 1.0);  // just in case
    data._AUbeatAndTempoValid = (noErr == CallHostBeatAndTempo(&data._beat, &data._tempo));
    data._AUmusicalTimeValid =
        (noErr == CallHostMusicalTimeLocation(&data._offsetToNextBeat, &data._musicalNumerator,
                                              &data._musicalDenominator, &data._currentDownBeat));
    // Get output buffer list and extract the i/o buffer pointers.
    // The loop is done so that an arbitrary number of output busses
    // with an arbitrary number of output channels is mapped onto a
    // continuous array of float buffers for the VST process function

    auto it_is = _plugin->AlwaysAudioThread();

    _processAdapter->process(data);

    {
      for (auto& i : _midi_outports)
      {
        if (i->hasEvents())
        {
          if (_midioutput_hostcallback.midiOutputCallback)
          {
            auto userd = _midioutput_hostcallback.userData;
            auto pktlist = i->getMIDIPacketList();
            auto fn = _midioutput_hostcallback.midiOutputCallback;
            [[maybe_unused]] OSStatus result = (*fn)(userd, &inTimeStamp, i->_auport, pktlist);
            assert(result == noErr);
          }
        }
        i->clear();
      }
    }
    // currently, the output events a     re processed directly
    //    _processAdapter->foreachOutputEvent([this]
    //                                        ()
    //                                        {}
    //                                        );
  }
  return noErr;
}

#define NOTIFYDIRECT

/*
 NOTIFYDIRECT defined means that we send the events within the AUDIO thread.
 This works now fine in all hosts. The actualy strategy to pass it to the UI thread
 and automate it from there does not work. Investigation is needed.
 */

void WrapAsAUV2::onBeginEdit(clap_id id)
{
#ifdef NOTIFYDIRECT
  AudioUnitEvent myEvent;
  myEvent.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
  myEvent.mArgument.mParameter.mAudioUnit = GetComponentInstance();
  myEvent.mArgument.mParameter.mParameterID = (AudioUnitParameterID)id;
  myEvent.mArgument.mParameter.mScope = kAudioUnitScope_Global;
  myEvent.mArgument.mParameter.mElement = 0;
  AUEventListenerNotify(NULL, NULL, &myEvent);

#else
  _queueToUI.push(BeginEvent(id));
#endif
}

void WrapAsAUV2::onPerformEdit(const clap_event_param_value_t* value)
{
#ifdef NOTIFYDIRECT
  Globals()->SetParameter(value->param_id, value->value);
  AudioUnitEvent myEvent;
  myEvent.mEventType = kAudioUnitEvent_ParameterValueChange;
  myEvent.mArgument.mParameter.mAudioUnit = GetComponentInstance();
  myEvent.mArgument.mParameter.mParameterID = (AudioUnitParameterID)value->param_id;
  myEvent.mArgument.mParameter.mScope = kAudioUnitScope_Global;
  myEvent.mArgument.mParameter.mElement = 0;
  AUEventListenerNotify(NULL, NULL, &myEvent);
#else

  _queueToUI.push(ValueEvent(value));
#endif
}

void WrapAsAUV2::onEndEdit(clap_id id)
{
#ifdef NOTIFYDIRECT
  AudioUnitEvent myEvent;
  myEvent.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
  myEvent.mArgument.mParameter.mAudioUnit = GetComponentInstance();
  myEvent.mArgument.mParameter.mParameterID = (AudioUnitParameterID)id;
  myEvent.mArgument.mParameter.mScope = kAudioUnitScope_Global;
  myEvent.mArgument.mParameter.mElement = 0;
  AUEventListenerNotify(NULL, NULL, &myEvent);
#else
  _queueToUI.push(EndEvent(id));
#endif
}

void WrapAsAUV2::onIdle()
{
  if (!_plugin) return;
  // run queue stuff
  queueEvent e;
  while (this->_queueToUI.pop(e))
  {
    switch (e._type)
    {
      case queueEvent::type::editstart:
      {
        AudioUnitEvent myEvent;
        myEvent.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
        myEvent.mArgument.mParameter.mAudioUnit = GetComponentInstance();
        myEvent.mArgument.mParameter.mParameterID = (AudioUnitParameterID)e._data._id;
        myEvent.mArgument.mParameter.mScope = kAudioUnitScope_Global;
        myEvent.mArgument.mParameter.mElement = 0;
        AUEventListenerNotify(NULL, NULL, &myEvent);
      }
      break;
      case queueEvent::type::editend:
      {
        AudioUnitEvent myEvent;
        myEvent.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
        myEvent.mArgument.mParameter.mAudioUnit = GetComponentInstance();
        myEvent.mArgument.mParameter.mParameterID = (AudioUnitParameterID)e._data._id;
        myEvent.mArgument.mParameter.mScope = kAudioUnitScope_Global;
        myEvent.mArgument.mParameter.mElement = 0;
        AUEventListenerNotify(NULL, NULL, &myEvent);
      }
      break;
      case queueEvent::type::editvalue:
      {
        Globals()->SetParameter(e._data._id, e._data._value.value);
        AudioUnitEvent myEvent;
        myEvent.mEventType = kAudioUnitEvent_ParameterValueChange;
        myEvent.mArgument.mParameter.mAudioUnit = GetComponentInstance();
        myEvent.mArgument.mParameter.mParameterID = (AudioUnitParameterID)e._data._id;
        myEvent.mArgument.mParameter.mScope = kAudioUnitScope_Global;
        myEvent.mArgument.mParameter.mElement = 0;
        AUEventListenerNotify(NULL, NULL, &myEvent);
      }
      break;
    }
  }

  if (_requestUICallback)
  {
    _requestUICallback = false;
    if (_plugin)
    {
      auto guarantee_mainthread = _plugin->AlwaysMainThread();
      _plugin->_plugin->on_main_thread(_plugin->_plugin);
    }
  }
}

OSStatus WrapAsAUV2::SaveState(CFPropertyListRef* ptPList)
{
  if (!ptPList) return kAudioUnitErr_InvalidParameter;

  if (!IsInitialized()) return kAudioUnitErr_Uninitialized;

  auto guarantee_mainthread = _plugin->AlwaysMainThread();

  if (!_plugin->_ext._state)
  {
    return AUBase::SaveState(ptPList);
  }
  else
  {
    Clap::StateMemento chunk;
    _plugin->_ext._state->save(_plugin->_plugin, chunk);

#if DICTIONARY_STREAM_FORMAT_JUCE
    auto err = ausdk::AUBase::SaveState(ptPList);
    if (err != noErr) return err;

    CFDataRef tData = CFDataCreate(0, (UInt8*)chunk.data(), chunk.size());
    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)*ptPList;

    CFDictionarySetValue(dict, CFSTR("jucePluginState"), tData);
    CFRelease(tData);
    chunk.clear();
#else
    CFDataRef tData = CFDataCreate(0, (UInt8*)chunk.data(), chunk.size());
    const AudioComponentDescription desc = GetComponentDescription();

    auto dict = ausdk::Owned<CFMutableDictionaryRef>::from_create(CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));

    // first step -> save the version to the data ref
    SInt32 value = 0;  // kCurrentSavedStateVersion;

    AddNumToDictionary(*dict, CFSTR(kAUPresetVersionKey), value);

    // second step -> save the component type, subtype, manu to the data ref
    value = static_cast<SInt32>(desc.componentType);
    AddNumToDictionary(*dict, CFSTR(kAUPresetTypeKey), value);

    value = static_cast<SInt32>(desc.componentSubType);
    AddNumToDictionary(*dict, CFSTR(kAUPresetSubtypeKey), value);

    value = static_cast<SInt32>(desc.componentManufacturer);
    AddNumToDictionary(*dict, CFSTR(kAUPresetManufacturerKey), value);

    CFDictionarySetValue(*dict, CFSTR(kAUPresetDataKey), tData);
    CFRelease(tData);
    chunk.clear();

    if (_current_program_name == nullptr)
    {
      _current_program_name = CFStringCreateWithCString(NULL, "Program", kCFStringEncodingUTF8);
    }
    // const char  *name = "blarb";
    // mPlugin->getProgramName(name);
    CFDictionarySetValue(*dict, CFSTR(kAUPresetNameKey), _current_program_name);

    *ptPList = static_cast<CFPropertyListRef>(dict.release());  // transfer ownership
#endif
  }

  return noErr;
}

OSStatus WrapAsAUV2::RestoreState(CFPropertyListRef plist)
{
  if (!plist) return kAudioUnitErr_InvalidParameter;

  if (!IsInitialized()) return kAudioUnitErr_Uninitialized;

  CFDictionaryRef tDict = CFDictionaryRef(plist);

  // Find 'data' key
  const void* pData = CFDictionaryGetValue(tDict, CFSTR(kAUPresetDataKey));
  if (!pData || CFGetTypeID(CFTypeRef(pData)) != CFDataGetTypeID()) return -1;

    /*
   * In the read side I fall through to default, whereas in the write
   * side I use an 'else' on the set of stream formats. This means
   * you at least try in case saved with an older wrapper version
   */
#if DICTIONARY_STREAM_FORMAT_JUCE
  /*
   * In the case when migrating from a JUCE AUv2 to a
   * clap-wrapper one, if you want to preserve state
   * you want to read the juce key from the dictionary.
   */
  CFDataRef juceData{nullptr};
  CFStringRef juceKey(
      CFStringCreateWithCString(kCFAllocatorDefault, "jucePluginState", kCFStringEncodingUTF8));
  bool valuePresent = CFDictionaryGetValueIfPresent(tDict, juceKey, (const void**)&juceData);
  CFRelease(juceKey);
  if (valuePresent && juceData)
  {
    LOGINFO("[clap-wrapper] Restoring from JUCE block");
    const int numBytes = (int)CFDataGetLength(juceData);
    if (numBytes > 0)
    {
      Clap::StateMemento chunk;
      UInt8* streamData = (UInt8*)(CFDataGetBytePtr(juceData));

      chunk.setData(streamData, numBytes);
      _plugin->_ext._state->load(_plugin->_plugin, chunk);
    }
    return noErr;
  }
#endif

  const void* pName = CFDictionaryGetValue(tDict, CFSTR(kAUPresetNameKey));
  if (pName)
  {
    _current_program_name = CFStringCreateCopy(NULL, (CFStringRef)pName);
  }

  CFDataRef tData = CFDataRef(pData);

  if (tData)
  {
    // Get length and ptr
    const long lLen = CFDataGetLength(tData);
    UInt8* pData = (UInt8*)(CFDataGetBytePtr(tData));
    if (lLen > 0 && pData)
    {
      Clap::StateMemento chunk;
      chunk.setData(pData, lLen);
      _plugin->_ext._state->load(_plugin->_plugin, chunk);
    }
  }
  return noErr;
}

bool WrapAsAUV2::ValidFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                             const AudioStreamBasicDescription& inNewFormat)
{
  if (!_plugin->_ext._audioports)
  {
    return false;
  }

  // Logic Pro does not call this in the main thread - so we just pretend..
  auto guarantee_mainthread = _plugin->AlwaysMainThread();

  auto ap = _plugin->_ext._audioports;
  auto pl = _plugin->_plugin;

  if (inScope == kAudioUnitScope_Input)
  {
    auto numAudioInputs = ap->count(pl, true);
    if (inElement >= numAudioInputs)
    {
      return false;
    }
    clap_audio_port_info inf;
    ap->get(pl, inElement, true, &inf);
    if (inNewFormat.mChannelsPerFrame == inf.channel_count)
    {
      // LOGINFO("In True");
      return true;
    }
  }
  else if (inScope == kAudioUnitScope_Output)
  {
    auto numAudioOutputs = ap->count(pl, false);
    if (inElement >= numAudioOutputs)
    {
      return false;
    }

    clap_audio_port_info inf;
    ap->get(pl, inElement, false, &inf);
    if (inNewFormat.mChannelsPerFrame == inf.channel_count)
    {
      // LOGINFO("Out True");
      return true;
    }
  }
  else if (inScope == kAudioUnitScope_Global)
  {
    return true;
  }
  //LOGINFO("False");
  return false;
}

OSStatus WrapAsAUV2::ChangeStreamFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                                        const AudioStreamBasicDescription& inPrevFormat,
                                        const AudioStreamBasicDescription& inNewFormat)
{
  // LOGINFO("ChangedStreamFormat called {} {}", inScope, inNewFormat.mChannelsPerFrame);
  auto res = ausdk::AUBase::ChangeStreamFormat(inScope, inElement, inPrevFormat, inNewFormat);

  return res;
}

UInt32 WrapAsAUV2::SupportedNumChannels(const AUChannelInfo** outInfo)
{
  if (cinfo.empty() && _plugin->_ext._audioports)
  {
    auto ap = _plugin->_ext._audioports;
    auto pl = _plugin->_plugin;
    auto numAudioInputs = ap->count(pl, true);
    auto numAudioOutputs = ap->count(pl, false);

    std::set<int> inSets, outSets;

    bool hasInMain{false};
    for (int i = 0; i < numAudioInputs; ++i)
    {
      clap_audio_port_info inf;
      ap->get(pl, i, true, &inf);
      inSets.insert(inf.channel_count);
      hasInMain |= (inf.flags & CLAP_AUDIO_PORT_IS_MAIN);
    }
    if (!hasInMain) inSets.insert(0);

    bool hasOutMain{false};
    for (int i = 0; i < numAudioOutputs; ++i)
    {
      clap_audio_port_info inf;
      ap->get(pl, i, false, &inf);
      outSets.insert(inf.channel_count);
      hasOutMain |= (inf.flags & CLAP_AUDIO_PORT_IS_MAIN);
    }
    if (!hasOutMain) outSets.insert(0);

    cinfo.clear();

    for (auto& iv : inSets)
    {
      for (auto& ov : outSets)
      {
        cinfo.emplace_back();
        cinfo.back().inChannels = iv;
        cinfo.back().outChannels = ov;
      }
    }
  }

  if (!outInfo) return (UInt32)cinfo.size();

  *outInfo = cinfo.data();
  return (UInt32)cinfo.size();
}

void WrapAsAUV2::PostConstructor()
{
  Base::PostConstructor();

  if (_plugin->_ext._audioports)
  {
    auto ap = _plugin->_ext._audioports;
    auto pl = _plugin->_plugin;

    auto numAudioInputs = ap->count(pl, true);
    auto numAudioOutputs = ap->count(pl, false);

    SetNumberOfElements(kAudioUnitScope_Input, numAudioInputs);
    Inputs().SetNumberOfElements(numAudioInputs);
    for (int i = 0; i < numAudioInputs; ++i)
    {
      clap_audio_port_info inf;
      ap->get(pl, i, true, &inf);
      auto b = CFStringCreateWithCString(nullptr, inf.name, kCFStringEncodingUTF8);
      Inputs().GetElement(i)->SetName(b);

      /*
      AudioChannelLayout layout;
      layout.mNumberChannelDescriptions = 1;
      layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
      memset(&layout, 0, sizeof(layout));
      Inputs().GetIOElement(i)->SetAudioChannelLayout(layout);
      */
    }

    SetNumberOfElements(kAudioUnitScope_Output, numAudioOutputs);
    Outputs().SetNumberOfElements(numAudioOutputs);
    for (int i = 0; i < numAudioOutputs; ++i)
    {
      clap_audio_port_info inf;
      ap->get(pl, i, false, &inf);
      auto b = CFStringCreateWithCString(nullptr, inf.name, kCFStringEncodingUTF8);
      Outputs().GetElement(i)->SetName(b);

      /*
      AudioChannelLayout layout;
      memset(&layout, 0, sizeof(layout));
      layout.mNumberChannelDescriptions = 1;
      layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
      Outputs().GetIOElement(i)->SetAudioChannelLayout(layout);
      */
    }
    LOGINFO("[clap-wrapper] PostConstructor: Ins={} Outs={}", numAudioInputs, numAudioOutputs);
  }
  // The else here would just set elements to 0,0 which is the default
  // therefore leave it un-elsed
}

UInt32 WrapAsAUV2::GetAudioChannelLayout(AudioUnitScope scope, AudioUnitElement element,
                                         AudioChannelLayout* outLayoutPtr, bool& outWritable)
{
  // TODO: This is never called so the layout is never found
  return Base::GetAudioChannelLayout(scope, element, outLayoutPtr, outWritable);
}

void WrapAsAUV2::send(const Clap::AUv2::clap_multi_event_t& event)
{
  // port index maps back to MIDI out
  auto type = event.header.type;
  switch (type)
  {
    case CLAP_EVENT_NOTE_ON:
    {
      auto portid = event.note.port_index;
      for (auto& i : _midi_outports)
      {
        if (i->_info.id == portid)
        {
          i->addNoteOn(event.note.channel, event.note.key, event.note.velocity * 127.f);
          break;
        }
      }
    }
    break;
    case CLAP_EVENT_NOTE_OFF:
    {
      auto portid = event.note.port_index;
      for (auto& i : _midi_outports)
      {
        if (i->_info.id == portid)
        {
          i->addNoteOff(event.note.channel, event.note.key, event.note.velocity * 127.f);
          break;
        }
      }
    }
    break;
    case CLAP_EVENT_MIDI:
    {
      auto portid = event.midi.port_index;
      for (auto& i : _midi_outports)
      {
        if (i->_info.id == portid)
        {
          i->addMIDI3Byte(event.midi.data);
          break;
        }
      }
    }
    break;
  }
#if 0
  MIDIEventList list;
  MIDIEventListInit(list, MIDIProtocolID protocolkMIDIProtocol_1_0);
  MIDIEventListAdd(list, <#ByteCount listSize#>, <#MIDIEventPacket * _Nonnull curPacket#>, <#MIDITimeStamp time#>, <#ByteCount wordCount#>, <#const UInt32 * _Nonnull words#>)
#endif
}

}  // namespace free_audio::auv2_wrapper
