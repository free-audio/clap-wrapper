#include "generated_entrypoints.hxx"


namespace free_audio::auv2_wrapper
{
bool WrapAsAUV2::initializeClapDesc()
{
  LOGINFO("[clap-wrapper auv2: id={} index: {}\n",_clapid,_idx);

  if (!_library.hasEntryPoint())
  {
    if (_clapname.empty())
    {
      std::cout << "[ERROR] _clapname (" << _clapname << ") empty and no internal entry point"
                << std::endl;
    }

    auto csp = Clap::getValidCLAPSearchPaths();
    auto it = std::find_if(csp.begin(), csp.end(),
                           [this](const auto &cs)
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
    for (auto *d : _library.plugins)
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

// the very very reduced state machine
OSStatus WrapAsAUV2::Initialize() {
  if (!_desc)
  {
    if (!initializeClapDesc())
    {
      return 1;
    }
    else
    {
      std::cout << "[clap-wrapper] auv2: Initialized '" << _desc->id << "' / '" << _desc->name
                << "' / '" << _desc->version << "'" << std::endl;
    }
  }
  if (!_desc) return 2;

  // first need to initialize the base to create
  // all elements needed
  auto res = Base::Initialize();
  if (res != noErr) return res;

  /*
   * ToDo: Stand up the host, create the plugin instance here
   */
  _plugin = Clap::Plugin::createInstance(_library._pluginFactory, _desc->id, this);

  // initialize will call IHost functions to set up busses etc, so be ready
  _plugin->initialize();
  
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

void WrapAsAUV2::setupWrapperSpecifics(const clap_plugin_t* plugin) {
  
  // TODO: if there are AUv2 specific extensions, they can be retrieved here
  // _auv2_specifics = (clap_plugin_as_auv2_t*)plugin->get_extension(plugin, CLAP_PLUGIN_AS_AUV2);
}

void WrapAsAUV2::setupAudioBusses(
    const clap_plugin_t* plugin,
    const clap_plugin_audio_ports_t* audioports) {
  
  auto numAudioInputs = audioports->count(plugin, true);
  auto numAudioOutputs = audioports->count(plugin, false);
  
  fprintf(stderr, "\tAUDIO in: %d, out: %d\n", (int)numAudioInputs, (int)numAudioOutputs);
  
  ausdk::AUBase::GetScope(kAudioUnitScope_Input).SetNumberOfElements(numAudioInputs);
  
  for ( decltype(numAudioInputs) i = 0 ; i < numAudioInputs ; ++i)
  {
    clap_audio_port_info_t info;
    if (audioports->get(plugin, i, true, &info))
    {
      auto& bus = Input(i);
      CFStringRef busNameString = CFStringCreateWithCString(NULL,info.name,kCFStringEncodingUTF8);
      bus.SetName (busNameString);
      CFRelease (busNameString);
      // addAudioBusFrom(&info, true);
      auto sf = bus.GetStreamFormat();
      sf.mChannelsPerFrame = info.channel_count;
      bus.SetStreamFormat(sf);
    }
  }
  
  ausdk::AUBase::GetScope(kAudioUnitScope_Output).Initialize(this, kAudioUnitScope_Global, numAudioOutputs);
  for ( decltype(numAudioOutputs) i = 0 ; i < numAudioOutputs ; ++i)
  {
    clap_audio_port_info_t info;
    if (audioports->get(plugin, i, false, &info))
    {
      auto& bus = Output(i);
      
      CFStringRef busNameString = CFStringCreateWithCString(NULL,info.name,kCFStringEncodingUTF8);
      bus.SetName (busNameString);
      CFRelease (busNameString);
      
      auto sf = bus.GetStreamFormat();
      sf.mChannelsPerFrame = info.channel_count;
      bus.SetStreamFormat(sf);
    }
  }

  ausdk::AUBase::ReallocateBuffers();
  
}  // called from initialize() to allow the setup of audio ports

void WrapAsAUV2::setupMIDIBusses(
    const clap_plugin_t* plugin,
    const clap_plugin_note_ports_t*
                     noteports)
{
  // TODO: figure out if MIDI is is prefered as CLAP or Notes
}

OSStatus WrapAsAUV2::Start() {
  // _plugin->start_processing();
  return noErr; // Base::Start();

}

OSStatus WrapAsAUV2::Stop() {
  // _plugin->stop_processing();
  return noErr; // Base::Stop();
  
}
void WrapAsAUV2::Cleanup() {
  _plugin->terminate();
  _plugin.reset();
  
  Base::Cleanup();
}

OSStatus WrapAsAUV2::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                     AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable)
{
  if (inScope == kAudioUnitScope_Global) {
    switch (inID) {
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
    default:
      break;
    }
  }
  return Base::GetPropertyInfo(inID,inScope,inElement,outDataSize,outWritable);
}

OSStatus WrapAsAUV2::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
  AudioUnitElement inElement, void* outData) {
  if (inScope == kAudioUnitScope_Global) {
    switch (inID) {
      case kMusicDeviceProperty_InstrumentCount:
        if (inScope != kAudioUnitScope_Global) {
          return kAudioUnitErr_InvalidScope;
        }
        *static_cast<UInt32*>(outData) = 0;
        return noErr;
        // return  GetInstrumentCount(*static_cast<UInt32*>(outData));
      case kAudioUnitProperty_BypassEffect:
        *static_cast<UInt32*>(outData) = (IsBypassEffect() ? 1 : 0); // NOLINT
        return noErr;
//    case kAudioUnitProperty_InPlaceProcessing:
//      *static_cast<UInt32*>(outData) = (mProcessesInPlace ? 1 : 0); // NOLINT
//      return noErr;
    default:
      break;
    }
  }
  return Base::GetProperty(inID,inScope,inElement,outData);
}
OSStatus WrapAsAUV2::SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
  AudioUnitElement inElement, const void* inData, UInt32 inDataSize)
{
  if (inScope == kAudioUnitScope_Global) {
    switch (inID) {
    case kAudioUnitProperty_BypassEffect: {
      if (inDataSize < sizeof(UInt32)) {
        return kAudioUnitErr_InvalidPropertyValue;
      }

      const bool tempNewSetting = *static_cast<const UInt32*>(inData) != 0;
      // we're changing the state of bypass
      if (tempNewSetting != IsBypassEffect()) {
        if (!tempNewSetting && IsBypassEffect() &&
          IsInitialized()) { // turning bypass off and we're initialized
          Reset(kAudioUnitScope_Global, 0);
        }
        SetBypassEffect(tempNewSetting);
      }
      return noErr;
    }
//    case kAudioUnitProperty_InPlaceProcessing:
//      mProcessesInPlace = *static_cast<const UInt32*>(inData) != 0;
//      return noErr;
    default:
      break;
    }
  }
  return Base::SetProperty(inID,inScope,inElement,inData,inDataSize);
}

OSStatus WrapAsAUV2::SetRenderNotification(AURenderCallback inProc, void* inRefCon){
  if ( _plugin)
  {
    _plugin->setBlockSizes(16, Base::GetMaxFramesPerSlice());
    _plugin->setSampleRate(Output(0).GetStreamFormat().mSampleRate);
    _plugin->activate();
    _plugin->start_processing();
  }
  return Base::SetRenderNotification(inProc, inRefCon);
}

OSStatus WrapAsAUV2::RemoveRenderNotification(AURenderCallback inProc, void* inRefCon)
{
  if ( _plugin)
  {
    _plugin->stop_processing();
    _plugin->deactivate();
  }
  return Base::RemoveRenderNotification(inProc, inRefCon);

}


}
