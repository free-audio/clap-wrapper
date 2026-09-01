#include "generated_entrypoints.hxx"
#include "detail/auv2/process.h"
#include <set>
#include <limits>
#include <cassert>
#include <algorithm>
#include <cmath>
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

////////////////////////////////////////////////////////////////////////////////
// Which of the four AU types the host instantiated us as. The generated entry
// point used to carry this, decided from the type string the build helper wrote
// into the Info.plist -- which had two problems. It knew only three of the four
// types, so an `aumf` silently became an instrument; and one entry point may now
// answer to more than one type, so there is no single build-time answer to bake.
// The component description is the host's own statement and is always right.
////////////////////////////////////////////////////////////////////////////////

AUV2_Type auv2TypeFromComponentType(OSType const componentType)
{
  switch (componentType)
  {
    case kAudioUnitType_Effect:
      return AUV2_Type::aufx_effect;
    case kAudioUnitType_MusicDevice:
      return AUV2_Type::aumu_musicdevice;
    case kAudioUnitType_MIDIProcessor:
      return AUV2_Type::aumi_noteeffect;
    case kAudioUnitType_MusicEffect:
      return AUV2_Type::aumf_musiceffect;
    default:
      return AUV2_Type::unknown;
  }
}

WrapAsAUV2::WrapAsAUV2(const std::string &clapname, const std::string &clapid, int idx,
                       AudioComponentInstance ci)
  : Base{ci, 0, 0}  // these elements are set correctly in ::PostConstructor
  , Clap::IHost()
  , Clap::IAutomation()
  , os::IPlugObject()
  , _autype(auv2TypeFromComponentType(GetComponentDescription().componentType))
  , _clapname{clapname}
  , _clapid{clapid}
  , _idx{idx}
  , _os_attached([this] { os::attach(this); }, [this] { os::detach(this); })
{
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
  if (!activateCLAP())
  {
    // The host settled on a main-bus format pair the plugin does not accept
    // (see activateCLAP). Refuse the initialization rather than render with
    // buffers sized differently from the plugin's ports.
    return kAudioUnitErr_FormatNotSupported;
  }

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
    auto p = _parametertree.find(inID);
    if (p != _parametertree.end())
    {
      // Fenced against onIdle(), the other consumer of the adapter's event
      // queue. Render() holding the lock is not enough on its own: AUBase calls
      // this from ScheduleParameter(), i.e. on the render thread but outside the
      // render, so without this the queue would have two writers.
      ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);
      if (_processAdapter)
      {
        auto &param = p->second.get()->info();
        _processAdapter->addParameterEvent(param, inValue, inBufferOffsetInFrames);
      }
    }

    // Nothing may ever render this: on an idle track process() is not coming,
    // and then onIdle() is the only thing that will hand it to the plugin.
    _requestedFlush = true;
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
  releaseHostMIDIOutput();
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

std::vector<clap_audio_port_configuration_request_t> WrapAsAUV2::mainBusConfigurationRequests(
    uint32_t mainInChannels, uint32_t mainOutChannels) const
{
  // One request per port, mirroring the VST3 wrapper's setBusArrangements
  // (wrapasvst3.cpp): main ports get the given counts, non-main ports keep
  // their current ones.
  std::vector<clap_audio_port_configuration_request_t> requests;
  auto addRequest = [&requests](bool isInput, uint32_t port, uint32_t channels)
  {
    clap_audio_port_configuration_request_t request{};
    request.is_input = isInput;
    request.port_index = port;
    request.channel_count = channels;
    request.port_type =
        (channels == 1) ? CLAP_PORT_MONO : ((channels == 2) ? CLAP_PORT_STEREO : nullptr);
    request.port_details = nullptr;
    requests.push_back(request);
  };

  for (size_t i = 0; i < _inputPortCache.size(); ++i)
    addRequest(true, static_cast<uint32_t>(i),
               _inputPortCache[i].isMain ? mainInChannels : _inputPortCache[i].channelCount);
  for (size_t i = 0; i < _outputPortCache.size(); ++i)
    addRequest(false, static_cast<uint32_t>(i),
               _outputPortCache[i].isMain ? mainOutChannels : _outputPortCache[i].channelCount);

  return requests;
}

bool WrapAsAUV2::applyConfigurationFromBusFormats()
{
  // Nothing to reconcile unless the PostConstructor probe found alternate
  // layouts: without them ValidFormat pinned every bus to its port's count.
  if (_channelCapsCache.empty() || !_plugin->_ext._audioports ||
      !_plugin->_ext._configurable_audio_ports)
    return true;

  // The channel counts the host settled on for the main busses. AU elements
  // map 1:1 to the CLAP ports scanned at PostConstructor, but a plugin that
  // changes its port *count* in apply_configuration can leave the caches
  // longer than the element scopes, and ausdk's Input()/Output() throw on an
  // element that does not exist - so never look past the element counts.
  bool mismatch = false;
  uint32_t mainInChannels = 0, mainOutChannels = 0;

  const size_t numInputElements = Inputs().GetNumberOfElements();
  const size_t numOutputElements = Outputs().GetNumberOfElements();

  for (size_t i = 0; i < _inputPortCache.size() && i < numInputElements; ++i)
  {
    if (!_inputPortCache[i].isMain) continue;
    mainInChannels = Input(static_cast<AudioUnitElement>(i)).GetStreamFormat().mChannelsPerFrame;
    mismatch |= (mainInChannels != _inputPortCache[i].channelCount);
    break;
  }
  for (size_t i = 0; i < _outputPortCache.size() && i < numOutputElements; ++i)
  {
    if (!_outputPortCache[i].isMain) continue;
    mainOutChannels = Output(static_cast<AudioUnitElement>(i)).GetStreamFormat().mChannelsPerFrame;
    mismatch |= (mainOutChannels != _outputPortCache[i].channelCount);
    break;
  }

  if (!mismatch) return true;

  auto requests = mainBusConfigurationRequests(mainInChannels, mainOutChannels);
  const bool applied = _plugin->_ext._configurable_audio_ports->apply_configuration(
      _plugin->_plugin, requests.data(), static_cast<uint32_t>(requests.size()));

  if (!applied)
  {
    LOGINFO("[clap-wrapper] could not apply an audio port configuration for {}/{} channels",
            mainInChannels, mainOutChannels);
    return false;
  }

  // The port layout changed: refresh the snapshots and the bus names/formats
  // from the rescanned ports. The plugin is still deactivated here, so the
  // scan is legal. Ports past the AU element counts (frozen at
  // PostConstructor) are cached but get no bus; the process adapter clamps
  // to the element counts the same way.
  auto ap = _plugin->_ext._audioports;
  auto pl = _plugin->_plugin;

  _inputPortCache.clear();
  const auto numAudioInputs = ap->count(pl, true);
  for (uint32_t i = 0; i < numAudioInputs; ++i)
  {
    clap_audio_port_info inf;
    ap->get(pl, i, true, &inf);
    _inputPortCache.push_back({inf.channel_count, (inf.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0});
    if (i < numInputElements) addInputBus(static_cast<int>(i), &inf);
  }

  _outputPortCache.clear();
  const auto numAudioOutputs = ap->count(pl, false);
  for (uint32_t i = 0; i < numAudioOutputs; ++i)
  {
    clap_audio_port_info inf;
    ap->get(pl, i, false, &inf);
    _outputPortCache.push_back({inf.channel_count, (inf.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0});
    if (i < numOutputElements) addOutputBus(static_cast<int>(i), &inf);
  }

  LOGINFO("[clap-wrapper] applied audio port configuration for {}/{} channels", mainInChannels,
          mainOutChannels);
  return true;
}

bool WrapAsAUV2::activateCLAP()
{
  if (_plugin)
  {
    assert(!_initialized);
    // Reconcile the host-chosen bus formats with the plugin's port layout
    // before anything reads the ports: main thread, plugin deactivated. A
    // failure here must fail the activation: ValidFormat can only vet each
    // scope on its own (AU hosts set formats one SetProperty at a time), so
    // the host can legally land on an input/output *pair* the plugin
    // rejects - and activating anyway would size the AU render buffers
    // differently from the plugin's ports, which is a heap overrun in the
    // render path.
    if (!applyConfigurationFromBusFormats())
    {
      return false;
    }

    auto maxSampleFrames = Base::GetMaxFramesPerSlice();
    auto minSampleFrames = (maxSampleFrames >= 16) ? 16 : 1;
    _plugin->setBlockSizes(minSampleFrames, maxSampleFrames);
    _plugin->setSampleRate(Output(0).GetStreamFormat().mSampleRate);

    // How long onIdle() waits before it decides the host has stopped rendering.
    // A fixed number of ticks cannot work: at 4096 frames and 44.1kHz a block is
    // 93ms, so a host that is rendering perfectly normally leaves gaps longer
    // than any small constant, and the idle tick would flush inside every one of
    // them -- stealing the scheduled automation SetParameter queues, offsets and
    // all. Three blocks is a gap no rendering host produces.
    const double sampleRate = std::max(Output(0).GetStreamFormat().mSampleRate, 1.0);
    const double blockMs = 1000.0 * static_cast<double>(maxSampleFrames) / sampleRate;
    const auto ticks = static_cast<uint32_t>(std::ceil(3.0 * blockMs / kIdleTickMs));
    _idleTicksBeforeFlush = std::max(kMinIdleTicksBeforeFlush, ticks);

    // the plugin's actually-declared audio port counts (0 when it has no
    // audio-ports extension); these may be fewer than the AU element counts
    uint32_t clapAudioInputs = 0, clapAudioOutputs = 0;
    if (_plugin->_ext._audioports)
    {
      clapAudioInputs = _plugin->_ext._audioports->count(_plugin->_plugin, true);
      clapAudioOutputs = _plugin->_ext._audioports->count(_plugin->_plugin, false);
    }

    {
      // Built and configured under the lock: every other path that queues onto
      // the adapter checks the pointer under it, and must not find one that is
      // half set up (setupProcessing clears the event queue as it goes).
      ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);
      if (!_processAdapter) _processAdapter = std::make_unique<Clap::AUv2::ProcessAdapter>();
      _processAdapter->setupProcessing(Inputs(), Outputs(), _plugin->_plugin, _plugin->_ext._params,
                                       this, &_parametertree, this, maxSampleFrames,
                                       _midi_preferred_dialect, _midi_supported_dialects,
                                       clapAudioInputs, clapAudioOutputs);
      // The deactivated-state flush adapter has no further use, and the gestures
      // it was tracking belong to the real one now.
      _flushAdapter.reset();
    }

    _plugin->activate();
    _plugin->start_processing();
    _initialized = true;
  }
  return true;
}

void WrapAsAUV2::deactivateCLAP()
{
  if (_plugin)
  {
    {
      // Under the lock: SetParameter and the MIDI entry points test this
      // pointer and then use it, and the idle tick may be inside a flush on it.
      ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);
      _initialized = false;
      _processAdapter.reset();
    }
    _plugin->stop_processing();
    _plugin->deactivate();
  }
}

// Only when the AU itself is going away. A restart the plugin asked for cycles
// the CLAP underneath a still-initialized AU, and the host does not reinstall
// what it handed us through SetProperty -- so dropping these there would lose
// MIDI output for the rest of the session.
void WrapAsAUV2::releaseHostMIDIOutput()
{
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
  ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);
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

    // This render carried whatever was queued in either direction, so the next
    // idle tick has nothing to make up for. See onIdle().
    _renderedSinceIdle = true;

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

void WrapAsAUV2::pushQueuedEventsToHost()
{
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
}

void WrapAsAUV2::onIdle()
{
  if (!_plugin) return;

  pushQueuedEventsToHost();

  if (_requestMarkDirty.exchange(false))
  {
    // The plugin's state no longer matches what the host last read. Apple's
    // v2->v3 bridge turns this into a KVO notification on the AUAudioUnit's
    // fullState, so v3 hosts hear it too.
    PropertyChanged(kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0);
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

  if (_requestRestart.exchange(false) && _plugin)
  {
    // AU has no host-facing "reinitialize me", but it needs none: the wrapper
    // owns the CLAP's activate/deactivate itself, so it can cycle it underneath
    // a still-initialized AU. The standalone does the same thing.
    auto guarantee_mainthread = _plugin->AlwaysMainThread();

    // The lock is held only long enough to close the door, not across the
    // rebuild. Render holds it for its whole body, so once it is acquired no
    // render is inside the process adapter; clearing _initialized under it
    // keeps the ones that follow out while the plugin is torn down and stood
    // back up. Those renders return without touching the buffers, exactly as
    // they do before the AU is initialized.
    bool wasInitialized;
    {
      ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);
      wasInitialized = _initialized.exchange(false);
    }

    if (wasInitialized)
    {
      deactivateCLAP();
      // Cannot fail for the format-pair reason Initialize guards against:
      // the formats have not changed since the last successful activation.
      // If it fails anyway, _initialized stays false and renders return
      // silence, the same state as before Initialize.
      if (!activateCLAP())
      {
        LOGINFO("[clap-wrapper] restart: could not reactivate the plugin");
      }
    }
  }

  // After the restart branch on purpose: a restart asked for in the same tick
  // ends with the plugin activated and processing, which is what a wake would
  // have been asking for anyway.
  if (_requestProcess.exchange(false) && _plugin)
  {
    auto guarantee_mainthread = _plugin->AlwaysMainThread();

    // AU offers a plugin nothing like this: there is no way to make the host
    // start pulling Render(). What the wrapper does own is the CLAP's own
    // activate/start_processing pair, which it drives from AU Initialize() --
    // so if the AU is initialized and the CLAP is not running underneath it,
    // stand it back up. No lock is needed to decide that: activateCLAP()
    // publishes _initialized last, and a render that reads it false returns
    // without touching the plugin, exactly as it does before the AU is
    // initialized at all.
    if (IsInitialized() && !_initialized)
    {
      activateCLAP();
    }

    // Whatever parameter traffic the request was really about is the flush
    // below's problem, in either direction: ask for one.
    _requestedFlush = true;
  }

  // The parameter flush.
  //
  // In CLAP a plugin can only push an output event -- a value its own editor
  // changed, the gesture around it -- from inside process() or flush(), and the
  // host's own parameter sets only reach the plugin the same way (SetParameter
  // queues them on the process adapter). Both directions therefore stop dead
  // whenever the host stops rendering, and AU hosts do stop: Logic will not run
  // a track it knows carries no signal, and an initialized unit can sit there
  // for minutes without a single Render() call. The VST3 SDK's AU wrapper and
  // JUCE's never meet this because neither routes automation through the audio
  // cycle at all -- they call AUParameterSet/AUEventListenerNotify straight from
  // the editor callback. A CLAP has no equivalent to call, so the wrapper has to
  // do the pulling.
  //
  // _requestedFlush says there is something to pull. The plugin sets it through
  // param_request_flush(), which it may call at any moment and in any state --
  // it is not a statement about processing, just "I have parameter events" --
  // and SetParameter sets it for the other direction. What is left to decide
  // here is only whether a render is going to do the job first.
  //
  // One empty tick is not proof the host has stopped, and guessing wrong costs
  // something: the host schedules automation through SetParameter with a buffer
  // offset, so a flush that beat the next render to the queue would deliver
  // those values stripped of their offsets. How long to wait cannot be a
  // constant either - at 4096 frames and 44.1kHz a block is 93ms, so a host
  // rendering perfectly normally leaves gaps longer than any small number of
  // 10ms ticks. activateCLAP() sizes the wait at three blocks, which no
  // rendering host produces; once one really has stopped, the counter stays
  // saturated and a request is served on the very next tick.
  if (_renderedSinceIdle.exchange(false))
  {
    _idleTicksSinceRender = 0;
  }
  else if (_idleTicksSinceRender < _idleTicksBeforeFlush)
  {
    ++_idleTicksSinceRender;
  }

  if (_requestedFlush && _idleTicksSinceRender >= _idleTicksBeforeFlush)
  {
    // Fences against Render() and against SetParameter, the queue's other
    // writer. While the plugin is active clap_plugin_params.flush() is
    // [audio-thread]; the lock is what makes the idle thread stand in for it.
    // It also keeps the single producer _queueToUI expects, since the output
    // events this flush pulls out go down the same path a render's would.
    ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);

    // Cleared before the work, not after: a request that arrives while the
    // plugin is inside flush() is about events this flush cannot have seen, and
    // has to survive into the next tick.
    _requestedFlush = false;

    if (_initialized && _processAdapter)
    {
      auto guarantee_audiothread = _plugin->AlwaysAudioThread();
      _processAdapter->flush();
    }
    else
    {
      // Deactivated: the real adapter does not exist, and flush() is
      // [main-thread] here.
      auto guarantee_mainthread = _plugin->AlwaysMainThread();
      flushParameters();
    }
  }

  // Announce anything the flush just produced in this tick rather than the next.
  pushQueuedEventsToHost();
}

// Builds a throwaway process adapter to flush against, for the deactivated case
// only: the real one lives between activateCLAP() and deactivateCLAP(), and
// clap_plugin_params.flush() is [main-thread] exactly while the plugin is
// inactive. Callers check _initialized under _processLock.
void WrapAsAUV2::flushParameters()
{
  if (!_plugin || !_plugin->_ext._params) return;

  // Kept between flushes rather than built per call, the way the VST3 wrapper
  // builds its throwaway: a gesture the plugin opens in one deactivated flush
  // and closes in the next has to find the same adapter, because that is where
  // the open was recorded -- a close arriving at a fresh one is dropped, and
  // the host stays armed on the parameter. activateCLAP() releases it.
  // No audio is involved: a zero numMaxSamples skips the silent-stream buffers,
  // and the plugin is handed no audio ports.
  if (!_flushAdapter)
  {
    _flushAdapter = std::make_unique<Clap::AUv2::ProcessAdapter>();
    _flushAdapter->setupProcessing(Inputs(), Outputs(), _plugin->_plugin, _plugin->_ext._params, this,
                                   &_parametertree, this, 0, _midi_preferred_dialect,
                                   _midi_supported_dialects, 0, 0);
  }
  _flushAdapter->flush();
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

  if (inNewFormat.mChannelsPerFrame == cache[inElement].channelCount) return true;

  // A main bus additionally accepts any channel count that one of the probed
  // configurable-audio-ports layouts offers on this scope — still validated
  // against the PostConstructor snapshots, never a live port scan. The
  // matching configuration is pushed into the plugin in activateCLAP
  // (applyConfigurationFromBusFormats), before it activates.
  if (cache[inElement].isMain)
  {
    // An alternate layout is accepted here on the promise that activateCLAP()
    // pushes it into the plugin before anything renders, and CLAP only allows
    // ports to be reconfigured while the plugin is deactivated. So once the AU
    // is initialized that promise cannot be kept: the only channel count still
    // valid is the one the ports actually have, which the equality above
    // already let through. A host wanting another layout has to uninitialize
    // first, which is the AU convention regardless.
    //
    // Accepting one anyway would be worse than refusing it. The AU element
    // would report a channel count the active plugin's port does not have, and
    // the process adapter - whose per-port pointer arrays were sized at
    // setupProcessing - would write past them on the next render. It is also
    // what auval tests: from an initialized 2/2 it sets the output to 1 and the
    // input to 3, and expects both to be turned away rather than to leave the
    // unit in a pair the plugin never advertised.
    if (IsInitialized()) return false;

    const bool isInput = (inScope == kAudioUnitScope_Input);
    for (const auto &caps : _channelCapsCache)
    {
      const uint32_t channels = isInput ? caps.inputChannels : caps.outputChannels;
      if (channels != 0 && channels == inNewFormat.mChannelsPerFrame) return true;
    }
  }
  return false;
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
  if (cinfo.empty() && !_channelCapsCache.empty())
  {
    // The PostConstructor probe found the plugin's accepted main-bus layouts
    // through clap.configurable-audio-ports: advertise exactly those (the
    // matching one is applied in activateCLAP once the host settles on it).
    for (const auto &caps : _channelCapsCache)
    {
      cinfo.emplace_back();
      cinfo.back().inChannels = static_cast<SInt16>(caps.inputChannels);
      cinfo.back().outChannels = static_cast<SInt16>(caps.outputChannels);
    }
  }
  else if (cinfo.empty() && isEffectFacade())
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

  // Probe the plugin's alternate main-bus layouts through
  // clap.configurable-audio-ports — the wrapper's only source of alternate
  // layouts, mirroring the VST3 wrapper's setBusArrangements. The plugin is
  // deactivated here, which is the state can_apply_configuration requires;
  // like the port scan above, the result is snapshotted and never queried
  // live afterwards. Every accepted pair is advertised through
  // SupportedNumChannels, accepted by ValidFormat, and applied in
  // activateCLAP once the host has settled on formats.
  // Gated on having output ports like the port-derived cross-product in
  // SupportedNumChannels: an input-only plugin renders through the
  // placeholder output element, and a probed pair would contradict that by
  // advertising 0 output channels.
  if (_plugin->_ext._configurable_audio_ports && !_outputPortCache.empty())
  {
    bool hasMainIn = false, hasMainOut = false;
    uint32_t currentIn = 0, currentOut = 0;
    for (const auto &port : _inputPortCache)
      if (port.isMain)
      {
        hasMainIn = true;
        currentIn = port.channelCount;
        break;
      }
    for (const auto &port : _outputPortCache)
      if (port.isMain)
      {
        hasMainOut = true;
        currentOut = port.channelCount;
        break;
      }

    // The probe varies only the main ports, and an AUChannelInfo pair can
    // only describe the main busses. A scope that has ports but none marked
    // main - a sidechain-only input, or a plugin that never sets
    // CLAP_AUDIO_PORT_IS_MAIN at all - can be neither varied nor described:
    // a probed pair would advertise 0 channels for a scope that does take
    // audio. Leave the cache empty in that case, which keeps the pre-probe
    // behaviour: SupportedNumChannels falls back to the port-derived
    // cross-product and ValidFormat pins every bus to its port's count.
    if ((_inputPortCache.empty() || hasMainIn) && (_outputPortCache.empty() || hasMainOut))
    {
      // The active layout is advertised unconditionally, whatever its
      // width: the grid below only *adds* alternates, so kMaxProbedChannels
      // never hides the plugin's own configuration (a 16-channel main
      // output stays advertised even though the grid stops at 8).
      _channelCapsCache.push_back({currentIn, currentOut});

      // A scope without any ports contributes the fixed count 0.
      const uint32_t minIn = currentIn ? 1 : 0, maxIn = currentIn ? kMaxProbedChannels : 0;
      const uint32_t minOut = currentOut ? 1 : 0, maxOut = currentOut ? kMaxProbedChannels : 0;

      for (uint32_t in = minIn; in <= maxIn; ++in)
      {
        for (uint32_t out = minOut; out <= maxOut; ++out)
        {
          if (in == currentIn && out == currentOut) continue;  // seeded above
          auto requests = mainBusConfigurationRequests(in, out);
          if (_plugin->_ext._configurable_audio_ports->can_apply_configuration(
                  _plugin->_plugin, requests.data(), static_cast<uint32_t>(requests.size())))
          {
            _channelCapsCache.push_back({in, out});
          }
        }
      }
      LOGINFO("[clap-wrapper] PostConstructor: {} probed main-bus channel layouts",
              _channelCapsCache.size());
    }
    else
    {
      LOGINFO(
          "[clap-wrapper] PostConstructor: not probing channel layouts, a scope has ports but no "
          "main port");
    }
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
