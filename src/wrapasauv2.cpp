#include "generated_entrypoints.hxx"
#include "detail/auv2/process.h"
#include <set>
#include <limits>
#include <cassert>
#include <Block.h>

extern bool fillAudioUnitCocoaView(AudioUnitCocoaViewInfo *viewInfo, std::shared_ptr<Clap::Plugin>);

namespace free_audio::auv2_wrapper
{

// Holds the library with plugins. Intentionally never destroyed: at process
// exit dyld finalizes the hosted .clap before this binary, so running
// clap_entry.deinit() from a static destructor reaches into a plugin whose
// own statics are already gone and aborts the host. AUv2 has no
// module-unload hook to release it from, so it is leaked and the OS reclaims
// it. See the same note in wrapasvst3_entry.cpp.
Clap::Library &_library = *new Clap::Library();

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
  const void *keys[] = {kCFUserNotificationAlertHeaderKey, kCFUserNotificationAlertMessageKey};
  const void *vals[] = {CFSTR("Test Foundation Message Box"), b};

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
                           [this](const auto &cs)
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

WrapAsAUV2::WrapAsAUV2(AUV2_Type type, const std::string &clapname, const std::string &clapid, int idx,
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
      if (_plugin)
      {
        _plugin->initialize();
        _os_attached.on();
      }
      else
      {
        std::cout << "[clap-wrapper] ERROR: the clap did not create an instance with id " << _desc->id
                  << std::endl;
        // this will exit in WrapAsAUV2::Initialize() with an error
        // if this happens, the wrapper or the clap plugin has a real bug
        _desc = nullptr;
      }
    }
  }
}

WrapAsAUV2::~WrapAsAUV2()
{
#if AUSDK_MIDI2_AVAILABLE
  if (auto blk = _midioutput_hosteventlistblock.exchange(nullptr)) Block_release(blk);
  for (auto blk : _retiredEventListBlocks) Block_release(blk);
  _retiredEventListBlocks.clear();
#endif
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

void WrapAsAUV2::setupWrapperSpecifics(const clap_plugin_t *plugin)
{
  // TODO: if there are AUv2 specific extensions, they can be retrieved here
  // _auv2_specifics = (clap_plugin_as_auv2_t*)plugin->get_extension(plugin, CLAP_PLUGIN_AS_AUV2);
}

void WrapAsAUV2::setupAudioBusses(const clap_plugin_t *plugin,
                                  const clap_plugin_audio_ports_t *audioports)
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

void WrapAsAUV2::setupMIDIBusses(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *noteports)
{
  // TODO: figure out if MIDI is is preferred as CLAP or Notes
  if (!noteports) return;
  auto numMIDIInPorts = noteports->count(plugin, true);
  auto numMIDIOutPorts = noteports->count(plugin, false);

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
      _midi_supported_dialects = info.supported_dialects;
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

void WrapAsAUV2::setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params)
{
  auto guarantee_mainthread = _plugin->AlwaysMainThread();
  // creating parameters.

  // Held for the whole rebuild, so that a host reading kAudioUnitProperty_ParameterInfo
  // on its own thread sees the tree either before or after this, never during.
  std::lock_guard<std::mutex> guard(_paramTreeMutex);

  // _clumps is not cleared here, and cannot be -- see the note on the class.
  _orderedParameterList.clear();
  _paramOrderingProvided = false;
  _bypassParamID = CLAP_INVALID_ID;
  auto *p = _plugin->_ext._params;
  if (p)
  {
    uint32_t numparams = p->count(_plugin->_plugin);

    // If the plugin provides a custom AUv2 param ordering, build an indirection array.
    // order[i] is the CLAP param index to use for AUv2 position i.
    std::vector<size_t> orderingStorage;
    const size_t *ordering = nullptr;
    auto *paramOrdering = _plugin->_ext._auv2_param_ordering;
    if (paramOrdering)
    {
      // Pre-fill with an out-of-range sentinel so we can detect untouched slots.
      orderingStorage.assign(numparams, std::numeric_limits<size_t>::max());
      if (paramOrdering->get_param_order(_plugin->_plugin, orderingStorage.data(), numparams))
      {
        // Sanity-check: every index 0..numparams-1 must appear exactly once.
        std::set<size_t> seen;
        bool orderingValid = true;
        for (size_t i = 0; i < numparams; ++i)
        {
          size_t idx = orderingStorage[i];
          if (idx >= numparams)
          {
            std::cout << "CLAP_PLUGIN_AUV2_PARAM_ORDERING: index " << idx << " at position " << i
                      << " is out of range [0, " << numparams << ")" << std::endl;
            orderingValid = false;
          }
          else if (!seen.insert(idx).second)
          {
            std::cout << "CLAP_PLUGIN_AUV2_PARAM_ORDERING: index " << idx << " appears more than once"
                      << std::endl;
            orderingValid = false;
          }
        }
        // Check for any indices that were never used (implies a duplicate stole their slot).
        for (size_t i = 0; i < numparams; ++i)
        {
          if (seen.find(i) == seen.end())
          {
            std::cout << "CLAP_PLUGIN_AUV2_PARAM_ORDERING: index " << i << " was never provided"
                      << std::endl;
            orderingValid = false;
          }
        }
        assert(orderingValid);
        if (orderingValid)
        {
          ordering = orderingStorage.data();
          _paramOrderingProvided = true;
        }
      }
    }

    clap_param_info_t paraminfo;
    for (uint32_t i = 0; i < numparams; ++i)
    {
      uint32_t clapIndex = ordering ? static_cast<uint32_t>(ordering[i]) : i;
      if (p->get_info(_plugin->_plugin, clapIndex, &paraminfo))
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
            _parametertree[paraminfo.id] =
                std::make_unique<Clap::AUv2::Parameter>(_plugin->_plugin, p, paraminfo);
          }
          else
          {
            piter->second->updateInfo(_plugin->_plugin, p, paraminfo);
          }
          // Assign the clump id here rather than leaving it to GetParameterInfo,
          // so that the ids follow parameter order and are settled before the
          // host is told there is anything to read.
          if (paraminfo.module[0] != 0)
          {
            _clumps.addClump(paraminfo.module);
          }
          if (paraminfo.flags & CLAP_PARAM_IS_BYPASS)
          {
            _bypassParamID = paraminfo.id;
            _isBypassed = (result >= 0.5 * (paraminfo.min_value + paraminfo.max_value));
          }
          Globals()->SetParameter(paraminfo.id, result);
          _orderedParameterList.push_back(static_cast<AudioUnitParameterID>(paraminfo.id));
        }
      }
    }
  }
}

OSStatus WrapAsAUV2::GetParameterList(AudioUnitScope inScope, AudioUnitParameterID *outParameterList,
                                      UInt32 &outNumParameters)
{
  if (inScope != kAudioUnitScope_Global || !_paramOrderingProvided)
  {
    return AUBase::GetParameterList(inScope, outParameterList, outNumParameters);
  }

  outNumParameters = static_cast<UInt32>(_orderedParameterList.size());
  if (outParameterList)
  {
    for (UInt32 i = 0; i < outNumParameters; ++i) outParameterList[i] = _orderedParameterList[i];
  }
  return noErr;
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
                                      AudioUnitParameterInfo &outParameterInfo)
{
  // const uint64_t stdflag = kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
  if (inScope == kAudioUnitScope_Global)
  {
    // A host may ask from any thread while setupParameters() is rebuilding the
    // tree on the main one -- the CFRetain below used to race the CFRelease in
    // Parameter::updateInfo().
    std::lock_guard<std::mutex> guard(_paramTreeMutex);
    auto pi = _parametertree.find(inParameterID);
    if (pi != _parametertree.end())
    {
      auto f = pi->second.get();
      const auto &info = f->info();

      outParameterInfo.flags = f->AudioUnitFlags();
      outParameterInfo.unit = f->AudioUnitUnit();

      // according to the documentation, the name field should be zeroed. In fact, AULab does display anything then.
      // strcpy(outParameterInfo.name, info.name);
      memset(outParameterInfo.name, 0, sizeof(outParameterInfo.name));

      CFRetain(f->CFString());
      outParameterInfo.cfNameString = f->CFString();
      outParameterInfo.minValue = info.min_value;
      outParameterInfo.maxValue = info.max_value;
      outParameterInfo.defaultValue = info.default_value;

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
        auto &param = p->second.get()->info();
        _processAdapter->addParameterEvent(param, inValue, inBufferOffsetInFrames);
      }
    }
  }
  return AUBase::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);
}

void WrapAsAUV2::SetBypassEffect(bool bypass)
{
  _isBypassed = bypass;
  if (_bypassParamID != CLAP_INVALID_ID)
  {
    // The value is copied out under the lock rather than the call being made
    // under it: SetParameter reaches the process adapter, which must not wait on
    // the main thread's rebuild.
    std::optional<double> value;
    {
      std::lock_guard<std::mutex> guard(_paramTreeMutex);
      auto p = _parametertree.find(_bypassParamID);
      if (p != _parametertree.end())
      {
        const auto &info = p->second->info();
        value = bypass ? info.max_value : info.min_value;
      }
    }
    if (value)
    {
      SetParameter(_bypassParamID, kAudioUnitScope_Global, 0, *value, 0);
    }
  }
}

OSStatus WrapAsAUV2::CopyClumpName(AudioUnitScope inScope, UInt32 inClumpID, UInt32 inDesiredNameLength,
                                   CFStringRef *outClumpName)
{
  if (outClumpName == nullptr) return kAudioUnitErr_InvalidParameter;
  if (inScope == kAudioUnitScope_Global)
  {
    // By value: the map this came out of belongs to the main thread and may be
    // rewritten the moment the lock inside getClump() is dropped.
    auto p = _clumps.getClump(inClumpID);
    if (p)
    {
      // A desired length of zero means "no limit", not "the empty string". The
      // SDK clamps kAudioUnitParameterName_Full (-1) to zero on the way in, and
      // that is how a host asks for the untruncated name.
      auto len = p->size();
      if (inDesiredNameLength > 0)
      {
        len = std::min(len, (size_t)inDesiredNameLength);
      }
      auto name =
          CFStringCreateWithBytes(NULL, (const UInt8 *)p->data(), len, kCFStringEncodingUTF8, false);
      // Truncation can land in the middle of a UTF-8 sequence, which fails the
      // conversion; a null here with noErr would be released by the host.
      if (name == nullptr) return kAudioUnitErr_InvalidPropertyValue;
      *outClumpName = name;
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
                                     AudioUnitElement inElement, UInt32 &outDataSize, bool &outWritable)
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
        return noErr;
        break;
#if AUSDK_MIDI2_AVAILABLE
      case kAudioUnitProperty_AudioUnitMIDIProtocol:
        outWritable = false;
        outDataSize = sizeof(SInt32);
        return noErr;
        break;
      case kAudioUnitProperty_MIDIOutputEventListCallback:
        outWritable = true;
        outDataSize = sizeof(AUMIDIEventListBlock);
        return noErr;
        break;
      case kAudioUnitProperty_HostMIDIProtocol:
        outWritable = true;
        outDataSize = sizeof(SInt32);
        return noErr;
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

OSStatus WrapAsAUV2::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                 AudioUnitElement inElement, void *outData)
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
        auto p = (AudioUnitParameterStringFromValue *)(outData);
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
        *static_cast<UInt32 *>(outData) = 0;
        if (_autype == AUV2_Type::aumu_musicdevice) return noErr;
        return kAudioUnitErr_InvalidProperty;
        // return  GetInstrumentCount(*static_cast<UInt32*>(outData));

      case kAudioUnitProperty_BypassEffect:
        *static_cast<UInt32 *>(outData) = (IsBypassEffect() ? 1 : 0);  // NOLINT
        return noErr;
        //    case kAudioUnitProperty_InPlaceProcessing:
        //      *static_cast<UInt32*>(outData) = (mProcessesInPlace ? 1 : 0); // NOLINT
        //      return noErr;
      case kAudioUnitProperty_ClapWrapper_UIConnection_id:
        _uiconn._plugin = _plugin.get();
        _uiconn._window = nullptr;
        _uiconn._registerWindow = [this](auto *x, auto *y)
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
        *static_cast<ui_connection *>(outData) = _uiconn;
        return noErr;

      case kAudioUnitProperty_CocoaUI:
        LOGINFO("[clap-wrapper] Property: kAudioUnitProperty_CocoaUI {}",
                (_plugin) ? "plugin" : "no plugin");
        if (_plugin && _plugin->_ext._gui &&
            (_plugin->_ext._gui->is_api_supported(_plugin->_plugin, CLAP_WINDOW_API_COCOA, false)))
        {
          fillAudioUnitCocoaView(((AudioUnitCocoaViewInfo *)outData), _plugin);
          LOGINFO("[clap-wrapper] kAudioUnitProperty_CocoaUI complete");
          return noErr;  // sizeof(AudioUnitCocoaViewInfo);
        }
        else
        {
          LOGINFO("[clap-wrapper] Mysterious: kAudioUnitProperty_CocoaUI although now plugin ext");
          fillAudioUnitCocoaView(((AudioUnitCocoaViewInfo *)outData), _plugin);
          return noErr;
        }
        return kAudioUnitErr_InvalidProperty;
        break;
      case kAudioUnitProperty_MIDIOutputCallbackInfo:
        if (_midi_outports.size() > 0)
        {
          CFMutableArrayRef callbackArray =
              CFArrayCreateMutable(NULL, _midi_outports.size(), &kCFTypeArrayCallBacks);

          for (const auto &portinfo : _midi_outports)
          {
            CFStringRef str =
                CFStringCreateWithCString(NULL, portinfo->_info.name, kCFStringEncodingUTF8);
            CFArrayAppendValue(callbackArray, str);
          }

          CFArrayRef array = CFArrayCreateCopy(NULL, callbackArray);

          *(CFArrayRef *)outData = array;

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
        *static_cast<UInt32 *>(outData) = 1;
        return noErr;
        break;
#if AUSDK_MIDI2_AVAILABLE
      case kAudioUnitProperty_AudioUnitMIDIProtocol:
        // the protocol we want our MIDI input delivered in: MIDI 2.0 only when
        // the hosted plugin actually prefers the MIDI2 note dialect, otherwise
        // MIDI 1.0 so everything flows through the (richer) MIDI1 translation.
        *static_cast<SInt32 *>(outData) =
            (_midi_preferred_dialect == CLAP_NOTE_DIALECT_MIDI2) ? kMIDIProtocol_2_0 : kMIDIProtocol_1_0;
        return noErr;
        break;
#endif
      default:
        break;
    }
  }
  return Base::GetProperty(inID, inScope, inElement, outData);
}
OSStatus WrapAsAUV2::SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
                                 AudioUnitElement inElement, const void *inData, UInt32 inDataSize)
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

        const bool tempNewSetting = *static_cast<const UInt32 *>(inData) != 0;
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
#if AUSDK_MIDI2_AVAILABLE
      case kAudioUnitProperty_MIDIOutputEventListCallback:
      {
        if (inDataSize < sizeof(AUMIDIEventListBlock)) return kAudioUnitErr_InvalidPropertyValue;
        AUMIDIEventListBlock newblk = nullptr;
        if (inData)
        {
          auto blk = *static_cast<const AUMIDIEventListBlock *>(inData);
          if (blk) newblk = Block_copy(blk);
        }
        // Render may be invoking the current block on the audio thread right now:
        // swap atomically and retire the old block instead of releasing it here.
        // Retired blocks are released in deactivateCLAP()/~WrapAsAUV2.
        auto oldblk = _midioutput_hosteventlistblock.exchange(newblk);
        if (oldblk) _retiredEventListBlocks.push_back(oldblk);
        return noErr;
      }
      case kAudioUnitProperty_HostMIDIProtocol:
        if (inDataSize < sizeof(SInt32)) return kAudioUnitErr_InvalidPropertyValue;
        _host_midi_protocol = static_cast<MIDIProtocolID>(*static_cast<const SInt32 *>(inData));
        return noErr;
        break;
#else
      case kAudioUnitProperty_MIDIOutputEventListCallback:
        break;
#endif
      case kAudioUnitProperty_MIDIOutputCallback:
        if (inDataSize < sizeof(AUMIDIOutputCallbackStruct)) return kAudioUnitErr_InvalidPropertyValue;

        if (inData)
        {
          _midioutput_hostcallback = *static_cast<const AUMIDIOutputCallbackStruct *>(inData);
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
        auto x = *static_cast<const UInt32 *>(inData);
        if (x > 0) LOGINFO("Host supports DualSchedulung Mode");
        _midi_dualscheduling_mode = (x != 0);
        return noErr;
      }
      break;
#endif
      case kMusicDeviceProperty_SupportsStartStopNote:
      {
        // TODO: we probably want to use start/stop note
        auto x = *static_cast<const UInt32 *>(inData);
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

OSStatus WrapAsAUV2::SetRenderNotification(AURenderCallback inProc, void *inRefCon)
{
  // activateCLAP();

  return Base::SetRenderNotification(inProc, inRefCon);
}

OSStatus WrapAsAUV2::RemoveRenderNotification(AURenderCallback inProc, void *inRefCon)
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

void WrapAsAUV2::addAudioBusFrom(int bus, const clap_audio_port_info_t *info, bool is_input)
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

void WrapAsAUV2::addInputBus(int bus, const clap_audio_port_info_t *info)
{
  auto &busref = Input(bus);

  CFStringRef busNameString = CFStringCreateWithCString(NULL, info->name, kCFStringEncodingUTF8);
  busref.SetName(busNameString);
  CFRelease(busNameString);

  auto sf = busref.GetStreamFormat();
  sf.mChannelsPerFrame = info->channel_count;
  busref.SetStreamFormat(sf);
}
void WrapAsAUV2::addOutputBus(int bus, const clap_audio_port_info_t *info)
{
  auto &busref = Output(bus);

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

    // the plugin's actually-declared audio port counts (0 when it has no
    // audio-ports extension); these may be fewer than the AU element counts
    uint32_t clapAudioInputs = 0, clapAudioOutputs = 0;
    if (_plugin->_ext._audioports)
    {
      clapAudioInputs = _plugin->_ext._audioports->count(_plugin->_plugin, true);
      clapAudioOutputs = _plugin->_ext._audioports->count(_plugin->_plugin, false);
    }

    _processAdapter->setupProcessing(Inputs(), Outputs(), _plugin->_plugin, _plugin->_ext._params, this,
                                     &_parametertree, this, maxSampleFrames, _midi_preferred_dialect,
                                     _midi_supported_dialects, clapAudioInputs, clapAudioOutputs);

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
#if AUSDK_MIDI2_AVAILABLE
  // No render can run once the AU is uninitialized: drop the event-list block
  // too (a stale block would shadow a legacy callback installed on the next
  // initialization) and release everything retired by SetProperty swaps.
  if (auto blk = _midioutput_hosteventlistblock.exchange(nullptr)) Block_release(blk);
  for (auto blk : _retiredEventListBlocks) Block_release(blk);
  _retiredEventListBlocks.clear();
#endif
}

OSStatus WrapAsAUV2::Render(AudioUnitRenderActionFlags &inFlags, const AudioTimeStamp &inTimeStamp,
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
      for (auto &i : _midi_outports)
      {
        if (i->hasEvents())
        {
#if AUSDK_MIDI2_AVAILABLE
          // prefer the modern UMP/EventList path when the host provided one
          // (load once: SetProperty may swap the block concurrently)
          if (auto evtblock = _midioutput_hosteventlistblock.load())
          {
            auto evtlist = i->getMIDIEventList();
            [[maybe_unused]] OSStatus result =
                evtblock(static_cast<AUEventSampleTime>(inTimeStamp.mSampleTime),
                         static_cast<uint8_t>(i->_auport), evtlist);
            assert(result == noErr);
          }
          else
#endif
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

void WrapAsAUV2::onPerformEdit(const clap_event_param_value_t *value)
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
        if (e._data._id == _bypassParamID)
        {
          // Decided under the lock, announced outside it: PropertyChanged runs
          // the host's listeners synchronously and they are free to come back in
          // through the property getters.
          std::optional<bool> bypassed;
          {
            std::lock_guard<std::mutex> guard(_paramTreeMutex);
            auto p = _parametertree.find(_bypassParamID);
            if (p != _parametertree.end())
            {
              const auto &info = p->second->info();
              bypassed = (e._data._value.value >= 0.5 * (info.min_value + info.max_value));
            }
          }
          if (bypassed && *bypassed != _isBypassed)
          {
            _isBypassed = *bypassed;
            PropertyChanged(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
          }
        }
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

OSStatus WrapAsAUV2::SaveState(CFPropertyListRef *ptPList)
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

    CFDataRef tData = CFDataCreate(0, (UInt8 *)chunk.data(), chunk.size());
    CFMutableDictionaryRef dict = (CFMutableDictionaryRef)*ptPList;

    CFDictionarySetValue(dict, CFSTR("jucePluginState"), tData);
    CFRelease(tData);
    chunk.clear();
#else
    CFDataRef tData = CFDataCreate(0, (UInt8 *)chunk.data(), chunk.size());
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
  const void *pData = CFDictionaryGetValue(tDict, CFSTR(kAUPresetDataKey));
  if (!pData || CFGetTypeID(CFTypeRef(pData)) != CFDataGetTypeID())
  {
    return -1;
  }
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
  bool valuePresent = CFDictionaryGetValueIfPresent(tDict, juceKey, (const void **)&juceData);
  CFRelease(juceKey);
  if (valuePresent && juceData)
  {
    LOGINFO("[clap-wrapper] Restoring from JUCE block");
    const int numBytes = (int)CFDataGetLength(juceData);
    if (numBytes > 0)
    {
      Clap::StateMemento chunk;
      UInt8 *streamData = (UInt8 *)(CFDataGetBytePtr(juceData));

      chunk.setData(streamData, numBytes);
      _plugin->_ext._state->load(_plugin->_plugin, chunk);
    }
    return noErr;
  }
#endif

  const void *pName = CFDictionaryGetValue(tDict, CFSTR(kAUPresetNameKey));
  if (pName)
  {
    _current_program_name = CFStringCreateCopy(NULL, (CFStringRef)pName);
  }

  CFDataRef tData = CFDataRef(pData);

  if (tData)
  {
    // Get length and ptr
    const long lLen = CFDataGetLength(tData);
    UInt8 *pData = (UInt8 *)(CFDataGetBytePtr(tData));
    if (lLen > 0 && pData)
    {
      Clap::StateMemento chunk;
      chunk.setData(pData, lLen);
      _plugin->_ext._state->load(_plugin->_plugin, chunk);
    }
  }
  return noErr;
}

bool WrapAsAUV2::isEffectFacade()
{
  // Both scopes are placeholder-only (no CLAP audio ports behind them, yet an AU
  // element exists). PostConstructor only adds a placeholder input bus for effect
  // component types, so this identifies an effect (aufx) facade for a plugin that
  // declares no audio ports at all.
  return _inputPortCache.empty() && _outputPortCache.empty() && Inputs().GetNumberOfElements() > 0 &&
         Outputs().GetNumberOfElements() > 0;
}

bool WrapAsAUV2::ValidFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                             const AudioStreamBasicDescription &inNewFormat)
{
  // Validate against the audio-port layout snapshotted at PostConstructor. We
  // must NOT scan the CLAP audio-ports extension here: hosts (e.g. Logic, auval)
  // call ValidFormat while the plugin is active, and CLAP only permits port
  // scanning while deactivated.
  if (inScope == kAudioUnitScope_Global)
  {
    return true;
  }

  const auto &cache = (inScope == kAudioUnitScope_Input) ? _inputPortCache : _outputPortCache;
  if (inElement >= cache.size())
  {
    // A placeholder silent bus (input or output) added in PostConstructor has no
    // CLAP port behind it. Only element 0 of a scope with no CLAP ports exists.
    if (!(cache.empty() && inElement == 0)) return false;

    // When BOTH scopes are placeholders the unit is an effect facade for a
    // plugin that declares no audio ports at all (see PostConstructor). Advertise
    // a fixed stereo layout and reject other channel counts, so auval / hosts
    // treat it as a plain stereo unit and don't init it into inconsistent
    // configurations. For a note device's lone silent output placeholder we stay
    // permissive so hosts may freely set the sample rate / channel count on that
    // unused bus (the channel count is all we gate here).
    if (isEffectFacade())
    {
      return inNewFormat.mChannelsPerFrame == kPlaceholderFacadeChannels;
    }
    return true;
  }
  return inNewFormat.mChannelsPerFrame == cache[inElement].channelCount;
}

OSStatus WrapAsAUV2::ChangeStreamFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                                        const AudioStreamBasicDescription &inPrevFormat,
                                        const AudioStreamBasicDescription &inNewFormat)
{
  // LOGINFO("ChangedStreamFormat called {} {}", inScope, inNewFormat.mChannelsPerFrame);
  auto res = ausdk::AUBase::ChangeStreamFormat(inScope, inElement, inPrevFormat, inNewFormat);

  return res;
}

UInt32 WrapAsAUV2::SupportedNumChannels(const AUChannelInfo **outInfo)
{
  // Built from the PostConstructor snapshot rather than a live port scan (see
  // ValidFormat) so this is safe to call while the plugin is active.
  if (cinfo.empty() && isEffectFacade())
  {
    // No CLAP audio ports at all: advertise the fixed stereo in/out layout of the
    // placeholder busses so auval / hosts treat this as a plain stereo unit.
    cinfo.emplace_back();
    cinfo.back().inChannels = kPlaceholderFacadeChannels;
    cinfo.back().outChannels = kPlaceholderFacadeChannels;
  }
  else if (cinfo.empty() && !_outputPortCache.empty())
  {
    std::set<int> inSets, outSets;

    bool hasInMain{false};
    for (const auto &p : _inputPortCache)
    {
      inSets.insert(p.channelCount);
      hasInMain |= p.isMain;
    }
    if (!hasInMain) inSets.insert(0);

    bool hasOutMain{false};
    for (const auto &p : _outputPortCache)
    {
      outSets.insert(p.channelCount);
      hasOutMain |= p.isMain;
    }
    if (!hasOutMain) outSets.insert(0);

    cinfo.clear();

    for (auto &iv : inSets)
    {
      for (auto &ov : outSets)
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
      _inputPortCache.push_back({inf.channel_count, (inf.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0});
      // SetNumberOfElements resets the bus, reapply the configuration.
      addInputBus(i, &inf);

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
      _outputPortCache.push_back({inf.channel_count, (inf.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0});
      // SetNumberOfElements resets the bus, reapply the configuration.
      addOutputBus(i, &inf);

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

  // A plugin can legitimately declare no audio output ports (a note effect such
  // as an arpeggiator, or a plugin with no audio-ports extension at all). The AU
  // model — and auval / Logic — still require at least one audio output element
  // to exist, otherwise Initialize fails with kAudioUnitErr_InvalidElement.
  // Present a placeholder silent output bus in that case; the CLAP is still told
  // its true (zero) audio-port count during processing, so no buffers are handed
  // to the plugin that it did not declare.
  if (Outputs().GetNumberOfElements() == 0)
  {
    SetNumberOfElements(kAudioUnitScope_Output, 1);
    Outputs().SetNumberOfElements(1);
    Outputs().GetElement(0)->SetName(CFSTR("Output"));
    LOGINFO("[clap-wrapper] PostConstructor: added placeholder silent output bus");
  }

  // Symmetric case for inputs: an effect with no audio input ports is not a valid
  // AudioUnit — auval / Logic require an input element on which a format can be
  // set, and reject the unit otherwise (e.g. a utility plugin that declares
  // clap.audio-ports but reports zero ports in both directions). Present a
  // placeholder silent input bus in that case. This must only be done for effect
  // component types (aufx / aumf): instruments and generators (aumu) and note
  // effects (aumi) legitimately have no audio input, and the AU type — not the
  // presence of note ports — is what hosts key on (a generator has neither audio
  // nor note input). As with the output, the CLAP is still told its true (zero)
  // input-port count during processing, so no input buffers are handed to a
  // plugin that did not declare them.
  const auto auType = GetComponentDescription().componentType;
  const bool isEffectType = (auType == kAudioUnitType_Effect) || (auType == kAudioUnitType_MusicEffect);
  if (isEffectType && Inputs().GetNumberOfElements() == 0)
  {
    SetNumberOfElements(kAudioUnitScope_Input, 1);
    Inputs().SetNumberOfElements(1);
    Inputs().GetElement(0)->SetName(CFSTR("Input"));
    LOGINFO("[clap-wrapper] PostConstructor: added placeholder silent input bus");
  }
}

UInt32 WrapAsAUV2::GetAudioChannelLayout(AudioUnitScope scope, AudioUnitElement element,
                                         AudioChannelLayout *outLayoutPtr, bool &outWritable)
{
  // TODO: This is never called so the layout is never found
  return Base::GetAudioChannelLayout(scope, element, outLayoutPtr, outWritable);
}

void WrapAsAUV2::send(const Clap::AUv2::clap_multi_event_t &event)
{
  // port index maps back to MIDI out
  auto type = event.header.type;
  switch (type)
  {
    case CLAP_EVENT_NOTE_ON:
    {
      auto portid = event.note.port_index;
      for (auto &i : _midi_outports)
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
      for (auto &i : _midi_outports)
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
      for (auto &i : _midi_outports)
      {
        if (i->_info.id == portid)
        {
          i->addMIDI3Byte(event.midi.data);
          break;
        }
      }
    }
    break;
    case CLAP_EVENT_NOTE_EXPRESSION:
    {
      // Pressure maps to poly/channel aftertouch and tuning to pitch bend;
      // expressions MIDI 1.0 cannot represent (volume/pan/vibrato/…) are dropped.
      uint8_t bytes[3];
      if (ClapWrapper::detail::shared::noteExpressionToMidi1(event.noteexpression, bytes) > 0)
      {
        auto portid = event.noteexpression.port_index;
        for (auto &i : _midi_outports)
        {
          if (i->_info.id == portid)
          {
            i->addMIDI3Byte(bytes);
            break;
          }
        }
      }
    }
    break;
    case CLAP_EVENT_MIDI_SYSEX:
    {
      const auto &sx = event.sysex;
      auto portid = sx.port_index;
      for (auto &i : _midi_outports)
      {
        if (i->_info.id == portid)
        {
          i->addSysEx(sx.buffer, sx.size);
          break;
        }
      }
    }
    break;
    case CLAP_EVENT_MIDI2:
    {
      // A plugin emitting raw UMP is rare. Down-convert MIDI 2.0 channel-voice
      // to MIDI 1.0 and route through the normal output; this works for both the
      // legacy callback and the UMP EventList path (which the framework then
      // up-converts to the host's negotiated protocol).
      uint8_t bytes[3];
      if (ClapWrapper::detail::shared::midi2ChannelVoiceToMidi1(event.midi2.data, bytes) > 0)
      {
        auto portid = event.midi2.port_index;
        for (auto &i : _midi_outports)
        {
          if (i->_info.id == portid)
          {
            i->addMIDI3Byte(bytes);
            break;
          }
        }
      }
    }
    break;
  }
}

}  // namespace free_audio::auv2_wrapper
