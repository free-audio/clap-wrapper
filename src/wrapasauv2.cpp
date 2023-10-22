#include "generated_entrypoints.hxx"
#include "detail/auv2/process.h"
#include "detail/os/osutil.h"

extern bool fillAudioUnitCocoaView(AudioUnitCocoaViewInfo* viewInfo);

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
  LOGINFO("[clap-wrapper auv2: id={} index: {}\n", _clapid, _idx);

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
                             return fs::is_directory(fp) && _library.load(fp.u8string().c_str());
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
  : Base{ci, 0, 1}, Clap::IHost(), _autype(type), _clapname{clapname}, _clapid{clapid}, _idx{idx}
{
  (void)_autype;  // TODO: will be used for dynamic property adaption
  if (!_desc)
  {
    if (initializeClapDesc())
    {
      std::cout << "[clap-wrapper] auv2: Initialized '" << _desc->id << "' / '" << _desc->name << "' / '"
                << _desc->version << "'" << std::endl;
      /*
       * ToDo: Stand up the host, create the plugin instance here
       */
      _plugin = Clap::Plugin::createInstance(_library._pluginFactory, _desc->id, this);
      // pffffrzz();
      _plugin->initialize();
    }
  }
}

WrapAsAUV2::~WrapAsAUV2()
{
  if (_plugin)
  {
    _plugin->terminate();
    _plugin.reset();
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
  auto keeper = _plugin->AlwaysMainThread();
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
// #if 0
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

  fprintf(stderr, "\tAUDIO in: %d, out: %d\n", (int)numAudioInputs, (int)numAudioOutputs);

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
    }
  }
  /*
  for (decltype(numMIDIInPorts) i = 0; i < numMIDIInPorts; ++i)
  {
    clap_note_port_info_t info;
    if (noteports->get(plugin, i, true, &info))
    {
      addMIDIBusFrom(&info, i, true);
    }
  }
   */
  // TODO: Outputs need the host MIDI list output thing
  /*
  for (decltype(numMIDIOutPorts) i = 0; i < numMIDIOutPorts; ++i)
  {
    clap_note_port_info_t info;
    if (noteports->get(plugin, i, false, &info))
    {
      addMIDIBusFrom(&info, i, false);
    }
  }
   */
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
  LOGINFO("Cleaning up Plugin");
  auto keeper = _plugin->AlwaysMainThread();
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
      case kMusicDeviceProperty_DualSchedulingMode:
        outWritable = true;
        outDataSize = sizeof(UInt32);
        return noErr;
        break;
      case kMusicDeviceProperty_SupportsStartStopNote:
        outWritable = true;
        outDataSize = sizeof(UInt32);
        return noErr;
        break;
      case kAudioUnitProperty_CocoaUI:
        outWritable = false;
        outDataSize = sizeof(struct AudioUnitCocoaViewInfo);
        LOGINFO("query Property Info: kAudioUnitProperty_CocoaUI");
        return noErr;
        break;
#if 0
        // TODO: for CLAPs that have MIDI output, we need these two properties.
      case kAudioUnitProperty_MIDIOutputCallbackInfo:
        return noErr;
        break;
      case kAudioUnitProperty_MIDIOutputCallback:
        outWritable = true;
        outDataSize = sizeof(AUMIDIOutputCallbackStruct);
        break;
#endif
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

ui_connection _uiconn;

OSStatus WrapAsAUV2::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                 AudioUnitElement inElement, void* outData)
{
  if (inScope == kAudioUnitScope_Global)
  {
    switch (inID)
    {
      case kMusicDeviceProperty_InstrumentCount:
        if (inScope != kAudioUnitScope_Global)
        {
          return kAudioUnitErr_InvalidScope;
        }
        // For a MusicDevice that doesn't support separate instruments (ie. is mono-timbral)
        // then this call should return an instrument count of zero and noErr
        *static_cast<UInt32*>(outData) = 0;
        return (_autype == AUV2_Type::aumu_musicdevice) ? noErr : kAudioUnitErr_InvalidProperty;
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
        *static_cast<ui_connection*>(outData) = _uiconn;
        return noErr;
      case kAudioUnitProperty_CocoaUI:
        LOGINFO("query Property: kAudioUnitProperty_CocoaUI {}", (_plugin) ? "plugin" : "no plugin");
        if (_plugin &&
            (_plugin->_ext._gui->is_api_supported(_plugin->_plugin, CLAP_WINDOW_API_COCOA, false)))
        {
          LOGINFO("now getting cocoa ui");
          fillAudioUnitCocoaView(((AudioUnitCocoaViewInfo*)outData));
          // *((AudioUnitCocoaViewInfo *)outData) = cocoaInfo;
          LOGINFO("query Property: kAudioUnitProperty_CocoaUI complete");
          return noErr;  // sizeof(AudioUnitCocoaViewInfo);
        }
        else
        {
          LOGINFO("query Property: kAudioUnitProperty_CocoaUI although now plugin ext");
          fillAudioUnitCocoaView(((AudioUnitCocoaViewInfo*)outData));
          return noErr;
        }
        return kAudioUnitErr_InvalidProperty;
        break;
      case kMusicDeviceProperty_DualSchedulingMode:
        // yes we do
        *static_cast<UInt32*>(outData) = 1;
        return noErr;
        break;
      case kMusicDeviceProperty_SupportsStartStopNote:
        // TODO: change this when figured out how the NoteParamsControlValue actually do work.

        *static_cast<UInt32*>(outData) = 0;
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
      case kMusicDeviceProperty_SupportsStartStopNote:
      {
        auto x = *static_cast<const UInt32*>(inData);
        (void)x;
        return noErr;
      }
      break;
      case kMusicDeviceProperty_DualSchedulingMode:
      {
        auto x = *static_cast<const UInt32*>(inData);
        if (x > 0) LOGINFO("Host supports DualSchedulung Mode");
        (void)x;
        return noErr;
      }
      break;

      default:
        break;
    }
  }
  return Base::SetProperty(inID, inScope, inElement, inData, inDataSize);
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

    _processAdapter->setupProcessing(Inputs(), Outputs(), _plugin->_plugin, _plugin->_ext._params,
                                     maxSampleFrames, _midi_preferred_dialect);

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
#if 0
    data._AUtransportValid = (noErr == CallHostTransportState(&data._isPlaying, &data._transportChanged, &data._currentSongPos, &data._isLooping, &data._cycleStart, &data._cycleEnd) );
    data._AUbeatAndTempoValid = (noErr ==
                               CallHostBeatAndTempo(&data._beat, &data._tempo));
    data._AUmusicalTimeValid = (noErr ==
                              CallHostMusicalTimeLocation(&data._offsetToNextBeat, &data._musicalNumerator, &data._musicalDenominator, &data._currentDownBeat));
#endif
    // Get output buffer list and extract the i/o buffer pointers.
    // The loop is done so that an arbitrary number of output busses
    // with an arbitrary number of output channels is mapped onto a
    // continuous array of float buffers for the VST process function

    _processAdapter->process(data);
  }
  return noErr;
}

}  // namespace free_audio::auv2_wrapper
