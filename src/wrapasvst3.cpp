#include "wrapasvst3.h"
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ustring.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <pluginterfaces/vst/ivstchannelcontextinfo.h>
#include <public.sdk/source/vst/utility/stringconvert.h>

// With 3.8.0 fstring is no longer up to snuff for wextra gcc so...
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <base/source/fstring.h>

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "detail/vst3/state.h"
#include "detail/vst3/process.h"
#include "detail/vst3/parameter.h"
#include "detail/clap/fsutil.h"
#include <locale>
#include <sstream>

#if VST_VERSION < 0x030800  // aka 3.8.anything
namespace stringconv = VST3::StringConvert;
#else
namespace stringconv = Steinberg::Vst::StringConvert;
#endif

// we need this lock free since we can request a gui resize from any thread in CLAP
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "compiler must ensure that std::atomic<uint32_t> is lock free");

#if WIN
#include <tchar.h>
#define S16(x) reinterpret_cast<const Steinberg::Vst::TChar *>(_T(x))
#endif
#if MAC
#define S16(x) u##x
#endif
#if LIN
#define S16(x) u##x
#endif

DEF_CLASS_IID(ARA::IPlugInEntryPoint)
DEF_CLASS_IID(ARA::IPlugInEntryPoint2)
DEF_CLASS_IID(Presonus::IGainReductionInfo)

#if 0
--- 8< ---
struct ClapHostExtensions
{
  static inline ClapAsVst3* self(const clap_host_t* host)
  {
    return static_cast<ClapAsVst3*>(host->host_data);
  }
  static void mark_dirty(const clap_host_t* host)
  {
    self(host)->mark_dirty();
  }
  const clap_host_state_t _state = {mark_dirty};
};
#endif

void utf8_to_utf16l(const char *utf8string, uint16_t *target, size_t targetsize)
{
  uint32_t codepoint = 0;
  size_t targetpos = 0;

  auto src = reinterpret_cast<const uint8_t *>(utf8string);
  size_t pos = 0;
  while (src[pos] && (targetpos < (targetsize - 2)))
  {
    auto byte = src[pos];

    if ((byte & 0b10000000) == 0b00000000)
    {
      codepoint = byte;
      pos += 1;
    }
    else
    {
      if (((byte & 0b11100000) == 0b11000000) && src[pos + 1])
      {
        codepoint = byte & 0b00011111;
        codepoint = (codepoint << 6) | ((src[pos + 1]) & 0b00111111);
        pos += 2;
      }
      else if (((byte & 0b11110000) == 0b11100000) && src[pos + 1] && src[pos + 2])
      {
        codepoint = byte & 0b00001111;
        codepoint = (codepoint << 6) | ((src[pos + 1] & 0b00111111));
        codepoint = (codepoint << 6) | ((src[pos + 2] & 0b00111111));
        pos += 3;
      }
      else if (((byte & 0b11111000) == 0b11110000) && src[pos + 1] && src[pos + 2] && src[pos + 3])
      {
        codepoint = byte & 0b00000111;
        codepoint = (codepoint << 6) | ((src[pos + 1] & 0b00111111));
        codepoint = (codepoint << 6) | ((src[pos + 2] & 0b00111111));
        codepoint = (codepoint << 6) | ((src[pos + 3] & 0b00111111));
        pos += 4;
      }
      else
      {
        // invalid UTF-8 sequence
        target[targetpos] = 0;
        return;
      }
    }
    {
      if (codepoint >= 0xD800 && codepoint <= 0xDFFF)
      {
        target[targetpos] = 0;
        return;
        // throw conversion_error("illegal UTF-32 codepoint (surrogat area)");
      }
      if (codepoint <= 0xFFFF)
      {
        target[targetpos++] = codepoint;
      }
      else
      {
        if (codepoint <= 0x10FFFF && (targetpos < (targetsize - 3)))
        {
          codepoint -= 0x10000;
          uint16_t highsurr = static_cast<uint16_t>((codepoint >> 10) + 0xD800);
          uint16_t lowsurr = static_cast<uint16_t>((codepoint & 0x3FF) + 0xDC00);
          target[targetpos++] = highsurr;
          target[targetpos++] = lowsurr;
        }
        else
        {
          target[targetpos] = 0;
          return;
        }
      }
    }
  }
  target[targetpos] = 0;
}

tresult PLUGIN_API ClapAsVst3::initialize(FUnknown *context)
{
  auto result = super::initialize(context);
  context->queryInterface(Vst::IHostApplication::iid, (void **)&vst3HostApplication);
  if (result == kResultOk)
  {
    if (!_plugin)
    {
      _plugin = Clap::Plugin::createInstance(*_library, _libraryIndex, this);
    }
    result = (_plugin && _plugin->initialize()) ? kResultOk : kResultFalse;
  }

  return result;
}

tresult PLUGIN_API ClapAsVst3::terminate()
{
  vst3HostApplication.reset();

  // Before anything else: the index lives in a module-wide cache and its
  // crawl thread holds our callback. Leaving it registered past here would let
  // it call into a half-terminated wrapper.
  if (_presetIndex && _presetIndexToken)
  {
    _presetIndex->removeCompletionListener(_presetIndexToken);
    _presetIndexToken = 0;
  }
  _presetIndex.reset();

  if (_plugin)
  {
    _os_attached.off();  // ensure we are detached
    if (_active)
    {
      // HOST has misbehaved
      _plugin->deactivate();
    }
    _plugin->terminate();
    _plugin.reset();
  }

  return super::terminate();
}

tresult PLUGIN_API ClapAsVst3::setActive(TBool state)
{
  if (state)
  {
    if (_active) return kResultFalse;

    for (auto i = 0U; i < audioInputs.size(); ++i)
    {
      _plugin->setBusActivation(true, i, audioInputs[i]->isActive());
    }

    for (auto i = 0U; i < audioOutputs.size(); ++i)
    {
      _plugin->setBusActivation(false, i, audioOutputs[i]->isActive());
    }

    if (!_plugin->activate()) return kResultFalse;

    _gesturedparameters.reserve(8192);

    _active = true;
    _processAdapter = new Clap::ProcessAdapter();
    _processAdapter->setupProcessing(
        _plugin->_plugin, _plugin->_ext._params, this->audioInputs, this->audioOutputs,
        this->_largestBlocksize, this->eventInputs.size(), this->eventOutputs.size(), parameters,
        componentHandler, this, _gesturedparameters,
        _expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_PRESSURE,
        _expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_TUNING);

    // the freshly created ProcessAdapter needs the current bus activation states
    updateAudioBusses();

    if (_missedLatencyRequest)
    {
      // cleared here, not just in getLatencySamples(): a host that does not answer
      // the restart by re-querying the latency would otherwise leave the flag set
      // and get a fresh kLatencyChanged out of every later setActive(true).
      _missedLatencyRequest = false;
      latency_changed();
    }

    _os_attached.on();
  }
  if (!state)
  {
    _os_attached.off();

    if (_active)
    {
      _plugin->deactivate();
    }
    _active = false;
    delete _processAdapter;
    _processAdapter = nullptr;

    // if the plugin didn't emit GESTURE_END for parameters still being edited,
    // end those edits towards the host now - otherwise the stale ids would
    // suppress host parameter changes after reactivation
    for (auto id : _gesturedparameters)
    {
      onEndEdit(id);
    }
    _gesturedparameters.clear();
  }
  return super::setActive(state);
}

tresult PLUGIN_API ClapAsVst3::process(Vst::ProcessData &data)
{
  if (!_active || !_processing)
  {
    return kNotInitialized;
  }

  ClapWrapper::detail::shared::SpinLockGuard spinLock(_processOrFlushLock);

  auto thisFn = _plugin->AlwaysAudioThread();

  // FIXME: At this transition we probably need to be careful that we aren't in a flush
  _processEverCalled = true;
  this->_processAdapter->process(data);
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

tresult PLUGIN_API ClapAsVst3::setState(IBStream *state)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  auto raise = _plugin->AlwaysMainThread();
  // a plugin may request a value rescan from within load(), which syncs the values for us
  _paramValuesSyncedDuringLoad = false;

  auto result =
      (_plugin->load(CLAPVST3StreamAdapter(state)) ? Steinberg::kResultOk : Steinberg::kResultFalse);

  // if the state was loaded correctly, values must be updated
  if (result == kResultOk && !_paramValuesSyncedDuringLoad)
  {
    syncParameterValuesFromClap();
  }
  return result;
}

void ClapAsVst3::syncParameterValuesFromClap()
{
  if (!_plugin->_ext._params) return;

  auto len = parameters.getParameterCount();
  for (decltype(len) i = 0; i < len; ++i)
  {
    auto p = static_cast<Vst3Parameter *>(parameters.getParameterByIndex(i));
    if (p->isMidi) continue;
    double val;
    if (_plugin->_ext._params->get_value(_plugin->_plugin, p->id, &val))
    {
      auto newval = p->asVst3Value(val);
      if (p->getNormalized() != newval)
      {
        p->setNormalized(newval);
      }
    }
  }
}

tresult PLUGIN_API ClapAsVst3::getState(IBStream *state)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  return (_plugin->save(CLAPVST3StreamAdapter(state)) ? Steinberg::kResultOk : Steinberg::kResultFalse);
}

uint32 PLUGIN_API ClapAsVst3::getLatencySamples()
{
  if (!_plugin->_ext._latency)
  {
    return 0;
  }
  if (!_active)
  {
    _missedLatencyRequest = true;
    return 0;
  }

  _missedLatencyRequest = false;
  return _plugin->_ext._latency->get(_plugin->_plugin);
}

uint32 PLUGIN_API ClapAsVst3::getTailSamples()
{
  // options would be kNoTail, number of samples or kInfiniteTail
  if (this->_active)
  {
    if (this->_plugin->_ext._tail)
    {
      auto tailsize = this->_plugin->_ext._tail->get(_plugin->_plugin);

      // Any value greater or equal to INT32_MAX implies infinite tail.
      if (tailsize >= INT32_MAX) return Vst::kInfiniteTail;
      return tailsize;
    }
  }
  return super::getTailSamples();
}

tresult PLUGIN_API ClapAsVst3::setupProcessing(Vst::ProcessSetup &newSetup)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  if (newSetup.symbolicSampleSize != Vst::kSample32)
  {
    return kResultFalse;
  }
  if (_plugin->_ext._render)
  {
    if (_plugin->_ext._render->has_hard_realtime_requirement(_plugin->_plugin) &&
        newSetup.processMode != Vst::kRealtime)
    {
      return kResultFalse;
    }
    clap_plugin_render_mode new_render_mode = CLAP_RENDER_REALTIME;
    if (newSetup.processMode == Vst::kOffline)
    {
      new_render_mode = CLAP_RENDER_OFFLINE;
    }
    // handling Vst::kPrefetch as Vst::kRealTime

    _plugin->_ext._render->set(_plugin->_plugin, new_render_mode);
  }
  _plugin->setSampleRate(newSetup.sampleRate);
  _plugin->setBlockSizes(newSetup.maxSamplesPerBlock, newSetup.maxSamplesPerBlock);

  _largestBlocksize = newSetup.maxSamplesPerBlock;

  return kResultOk;
}
tresult PLUGIN_API ClapAsVst3::setProcessing(TBool state)
{
  std::lock_guard x(_processingLock);
  tresult result = kResultOk;

  if (state)
  {
    if (!_processing)
    {
      _processing = true;

      result = (_plugin->start_processing() ? Steinberg::kResultOk : Steinberg::kResultFalse);
    }
  }
  else
  {
    if (_processing)
    {
      _processing = false;
      _plugin->stop_processing();

      // VST3 has no specific reset - but it should happen when setprocessing is being called
      // https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Workflow+Diagrams/Audio+Processor+Call+Sequence.html
      _plugin->reset();

      _processEverCalled = false;
    }
  }
  return result;
}

tresult PLUGIN_API ClapAsVst3::setBusArrangements(Vst::SpeakerArrangement *inputs, int32 numIns,
                                                  Vst::SpeakerArrangement *outputs, int32 numOuts)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  if (!_plugin->_ext._audioports)
  {
    return kResultFalse;
  }

  auto raise = _plugin->AlwaysMainThread();

  // if we have configurable-audio-ports, ask the plugin to set the requested arrangement
  if (_plugin->_ext._configurable_audio_ports)
  {
    std::vector<clap_audio_port_configuration_request_t> requests;

    for (int i = 0; i < numIns + numOuts; ++i)
    {
      clap_audio_port_configuration_request_t request;

      request.is_input = i < numIns;
      request.port_index = i < numIns ? i : (i - numIns);
      auto arrangement = i < numIns ? inputs[i] : outputs[i - numIns];

      switch (arrangement)
      {
        case Vst::SpeakerArr::kMono:
          request.channel_count = 1;
          request.port_type = CLAP_PORT_MONO;
          request.port_details = nullptr;
          break;
        case Vst::SpeakerArr::kStereo:
          request.channel_count = 2;
          request.port_type = CLAP_PORT_STEREO;
          request.port_details = nullptr;
          break;
        default:
          request.channel_count = static_cast<uint32_t>(Vst::SpeakerArr::getChannelCount(arrangement));
          request.port_type = nullptr;
          request.port_details = nullptr;
          break;
      }

      requests.push_back(request);
    }

    if (_plugin->_ext._configurable_audio_ports->apply_configuration(
            _plugin->_plugin, requests.data(), static_cast<uint32_t>(requests.size())))
    {
      setupAudioBusses(_plugin->_plugin, _plugin->_ext._audioports);
      return super::setBusArrangements(inputs, numIns, outputs, numOuts);
    }
  }

  // otherwise we just make sure that the requested arrangements matches the current layout
  {
    int32_t inc = _plugin->_ext._audioports->count(_plugin->_plugin, true);
    int32_t ouc = _plugin->_ext._audioports->count(_plugin->_plugin, false);
    if (inc != numIns || ouc != numOuts) return kResultFalse;

    for (int i = 0; i < numIns; ++i)
    {
      clap_audio_port_info_t info;
      _plugin->_ext._audioports->get(_plugin->_plugin, i, true, &info);
      if (static_cast<uint32_t>(Vst::SpeakerArr::getChannelCount(inputs[i])) != info.channel_count)
        return kResultFalse;
    }

    for (int i = 0; i < numOuts; ++i)
    {
      clap_audio_port_info_t info;
      _plugin->_ext._audioports->get(_plugin->_plugin, i, false, &info);
      if (static_cast<uint32_t>(Vst::SpeakerArr::getChannelCount(outputs[i])) != info.channel_count)
        return kResultFalse;
    }
  }

  return super::setBusArrangements(inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API ClapAsVst3::getBusArrangement(Vst::BusDirection dir, int32 index,
                                                 Vst::SpeakerArrangement &arr)
{
  return super::getBusArrangement(dir, index, arr);
}

tresult PLUGIN_API ClapAsVst3::setComponentState(IBStream * /*state*/)
{
  // this should satisfy Hosts that want to be satisfied
  return kResultOk;
}

IPlugView *PLUGIN_API ClapAsVst3::createView(FIDString /*name*/)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  if (_plugin->_ext._gui)
  {
    clearContextMenu();
    if (_wrappedview == nullptr)
    {
      _wrappedview = new WrappedView(
          _plugin->_plugin, _plugin->_ext._gui, [this]() { clearContextMenu(); },
          [this](bool everCreated)
          {
            if (everCreated)
            {
#if LIN
              // the host calls the destructor, the wrapper just removes its pointer
              detachTimers(_wrappedview->getRunLoop());
              detachPosixFD(_wrappedview->getRunLoop());
              _iRunLoop = nullptr;

              // the editor is going away, and with it the only main thread the
              // host was ever going to hand us -- wake the helper back up
              os::idleSourceChanged();
#endif

              clearContextMenu();
            }
            this->_wrappedview = nullptr;
          },
          [this]()
          {

#if LIN
            if (auto *const runLoop = _wrappedview->getRunLoop())
            {
              attachTimers(runLoop);
              attachPosixFD(runLoop);
            }
            else if (_iRunLoop)
            {
              // the host took the frame back but kept the view: unregister
              // while the old run loop is still alive, then hand the idle back
              // to the helper thread
              detachTimers(_iRunLoop);
              detachPosixFD(_iRunLoop);
              _iRunLoop = nullptr;
              os::idleSourceChanged();
            }
#else
            (void)this;  // silence warning on non-linux
#endif
          });
    }
    return _wrappedview;
  }
  return nullptr;
}

tresult PLUGIN_API ClapAsVst3::getParamStringByValue(Vst::ParamID id, Vst::ParamValue valueNormalized,
                                                     Vst::String128 string)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  auto param = (Vst3Parameter *)this->getParameterObject(id);
  auto val = param->asClapValue(valueNormalized);

  if (param->getInfo().flags & Vst::ParameterInfo::kIsProgramChange)
  {
    std::string program("Program ");
    program.append(std::to_string((int)val));
    UString wrapper(&string[0], str16BufferSize(Steinberg::Vst::String128));

    wrapper.assign(program.c_str(), (Steinberg::int32)(program.size() + 1));
    return kResultOk;
  }

  if (param->isMidi)
  {
    auto r = std::to_string((int)val);

    // usually we try to avoid UString assignment, but here it is okay.
    UString wrapper(&string[0], str16BufferSize(Steinberg::Vst::String128));

    wrapper.assign(r.c_str(), (Steinberg::int32)(r.size() + 1));

    return kResultOk;
  }

  char outbuf[128];
  memset(outbuf, 0, sizeof(outbuf));

  auto raise = _plugin->AlwaysMainThread();
  if (this->_plugin->_ext._params->value_to_text(_plugin->_plugin, param->id, val, outbuf, 127))
  {
    utf8_to_utf16l(outbuf, (uint16_t *)&string[0], str16BufferSize(Steinberg::Vst::String128));

    return kResultOk;
  }
  return super::getParamStringByValue(id, valueNormalized, string);
}

tresult PLUGIN_API ClapAsVst3::getParamValueByString(Vst::ParamID id, Vst::TChar *string,
                                                     Vst::ParamValue &valueNormalized)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  auto param = (Vst3Parameter *)this->getParameterObject(id);
  Steinberg::String m(string);
  char inbuf[128];
  m.copyTo8(inbuf, 0, 128);
  double out = 0.;
  if (param->isMidi)
  {
    return Steinberg::kResultFalse;
  }
  auto raise = _plugin->AlwaysMainThread();
  if (this->_plugin->_ext._params->text_to_value(_plugin->_plugin, param->id, inbuf, &out))
  {
    valueNormalized = param->asVst3Value(out);
    return kResultOk;
  }
  return Steinberg::kResultFalse;
}

tresult PLUGIN_API ClapAsVst3::activateBus(Vst::MediaType type, Vst::BusDirection dir, int32 index,
                                           TBool state)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  return super::activateBus(type, dir, index, state);
}

tresult PLUGIN_API ClapAsVst3::setIoMode(Vst::IoMode mode)
{
#if 0  // disabled for now
  // since there is always the override in setupProcessing, setting the mode here
  // does not make much sense - even for a VST3
  // so for now this stays kUnimplemented until we find a proper use case

  auto rext = _plugin->_ext._render;

  if (rext)
  {
    auto mainthread = _plugin->AlwaysMainThread();

    bool realtime_only = rext->has_hard_realtime_requirement(_plugin->_plugin);
    switch (mode)
    {
      case Vst::kOfflineProcessing:
        if (realtime_only) return kResultFalse;
        return (rext->set(_plugin->_plugin, CLAP_RENDER_OFFLINE)) ? kResultOk : kResultFalse;
        break;
      case Vst::kSimple:
      case Vst::kAdvanced:
        // both does not make any difference
        return (rext->set(_plugin->_plugin, CLAP_RENDER_REALTIME)) ? kResultOk : kResultFalse;
        break;
      default:
        return kNotImplemented;
    }
  }
#endif
  return super::setIoMode(mode);
}

//-----------------------------------------------------------------------------

tresult PLUGIN_API ClapAsVst3::setComponentHandler(Vst::IComponentHandler *handler)
{
  // [main-thread] -- must not run while the Linux helper thread is inside this
  // plug-in's idle. \see _mainThreadLock
  std::lock_guard<std::recursive_mutex> mainThreadGuard(_mainThreadLock);
  componentHandler3.reset();

  // the base class extracts IComponentHandler and IComponentHandler2
  auto result = super::setComponentHandler(handler);
  // but for context menus IComponentHandler3 is needed, so it is being retrieved here.
  if (componentHandler && result == kResultOk)
  {
    this->componentHandler->queryInterface(Vst::IComponentHandler3::iid, (void **)&componentHandler3);
  }

  return result;
}

//-----------------------------------------------------------------------------

tresult PLUGIN_API ClapAsVst3::getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                           Vst::CtrlNumber midiControllerNumber,
                                                           Vst::ParamID &id /*out*/)
{
  // for my first Event bus and for MIDI channel 0 and for MIDI CC Volume only
  if (busIndex == 0)  // && channel == 0) // && midiControllerNumber == Vst::kCtrlVolume)
  {
    if (midiControllerNumber < Vst::kCountCtrlNumber)  // with program change
    {
      id = _IMidiMappingIDs[channel][midiControllerNumber];
      return kResultTrue;
    }
  }
  return kResultFalse;
}

//----from IInfoListener--------------------------------------
tresult PLUGIN_API ClapAsVst3::setChannelContextInfos(Vst::IAttributeList *list /*in*/)
{
  if (!_plugin->_ext._trackinfo) return kResultFalse;
  if (!_trackInfo) _trackInfo = std::make_unique<clap_track_info_t>();
  _trackInfo->flags = 0;

  int64_t color = 0;
  if (list->getInt(Vst::ChannelContext::kChannelColorKey, color) == kResultOk)
  {
    _trackInfo->flags |= CLAP_TRACK_INFO_HAS_TRACK_COLOR;
    _trackInfo->color = clap_color{
        Vst::ChannelContext::GetAlpha((uint32_t)color), Vst::ChannelContext::GetRed((uint32_t)color),
        Vst::ChannelContext::GetGreen((uint32_t)color), Vst::ChannelContext::GetBlue((uint32_t)color)};
  }

  Steinberg::Vst::TChar name[CLAP_NAME_SIZE];
  if (list->getString(Vst::ChannelContext::kChannelNameKey, name, sizeof(name)) == kResultOk)
  {
    _trackInfo->flags |= CLAP_TRACK_INFO_HAS_TRACK_NAME;
    Steinberg::String(name, CLAP_NAME_SIZE).copyTo8(_trackInfo->name, 0, CLAP_NAME_SIZE);
  }

  _plugin->_ext._trackinfo->changed(_plugin->_plugin);
  return kResultOk;
}

#if 1
//----from INoteExpressionController-------------------------
/** Returns number of supported note change types for event bus index and channel. */
int32 ClapAsVst3::getNoteExpressionCount(int32 busIndex, int16 channel)
{
  if (busIndex == 0 && channel == 0)
  {
    return _noteExpressions.getNoteExpressionCount();
  }
  return 0;
}

/** Returns note change type info. */
tresult ClapAsVst3::getNoteExpressionInfo(int32 busIndex, int16 channel, int32 noteExpressionIndex,
                                          Vst::NoteExpressionTypeInfo &info /*out*/)
{
  if (busIndex == 0 && channel == 0)
  {
    return _noteExpressions.getNoteExpressionInfo(noteExpressionIndex, info);
  }
  return Steinberg::kResultFalse;
}

/** Gets a user readable representation of the normalized note change value. */
tresult ClapAsVst3::getNoteExpressionStringByValue(int32 /*busIndex*/, int16 /*channel*/,
                                                   Vst::NoteExpressionTypeID id,
                                                   Vst::NoteExpressionValue valueNormalized /*in*/,
                                                   Vst::String128 string /*out*/)
{
  return _noteExpressions.getNoteExpressionStringByValue(id, valueNormalized, string);
}

/** Converts the user readable representation to the normalized note change value. */
tresult ClapAsVst3::getNoteExpressionValueByString(int32 /*busIndex*/, int16 /*channel*/,
                                                   Vst::NoteExpressionTypeID id,
                                                   const Vst::TChar *string /*in*/,
                                                   Vst::NoteExpressionValue &valueNormalized /*out*/)
{
  return _noteExpressions.getNoteExpressionValueByString(id, string, valueNormalized);
}

#endif

tresult ClapAsVst3::getUnitByBus(Vst::MediaType type, Vst::BusDirection dir, int32 busIndex,
                                 int32 channel, Vst::UnitID &unitId /*out*/)
{
  if (type == Vst::MediaTypes::kEvent && dir == Vst::BusDirections::kInput)
  {
    if (busIndex == 0)
    {
      if ((channel >= 0) && (channel < (Steinberg::int32)_MIDIUnits.size()))
      {
        unitId = _MIDIUnits[channel];
        return kResultTrue;
      }
    }
  }
  return kResultFalse;
}

tresult ClapAsVst3::executeMenuItem(int32 tag)
{
  return kResultOk;
}

ARAFactoryPtr PLUGIN_API ClapAsVst3::getFactory()
{
  LOGDETAIL("-> ARA::IPlugInEntryPoint::getFactory");
  if (_plugin->_ext._ara)
  {
    return _plugin->_ext._ara->get_factory(_plugin->_plugin);
  }
  return nullptr;
}

ARAPlugInExtensionInstancePtr PLUGIN_API
ClapAsVst3::bindToDocumentController(ARADocumentControllerRef documentControllerRef)
{
  LOGDETAIL("-> ARA::IPlugInEntryPoint::bindToDocumentController (!!! DEPRECATED !!!)");
  // "call is deprecated in ARA 2, host must not call this"
  return nullptr;
}

ARAPlugInExtensionInstancePtr PLUGIN_API ClapAsVst3::bindToDocumentControllerWithRoles(
    ARADocumentControllerRef documentControllerRef, ARAPlugInInstanceRoleFlags knownRoles,
    ARAPlugInInstanceRoleFlags assignedRoles)
{
  LOGDETAIL("-> ARA::IPlugInEntryPoint2::bindToDocumentControllerWithRoles");
  if (_plugin->_ext._ara)
  {
    return _plugin->_ext._ara->bind_to_document_controller(_plugin->_plugin, documentControllerRef,
                                                           knownRoles, assignedRoles);
  }
  return nullptr;
}

// TODO: surround extension support
static Vst::SpeakerArrangement speakerArrFromPortType(const char *port_type, uint32_t channel_count)
{
  if (!port_type)
  {
    switch (channel_count)
    {
      case 0:
        return Vst::SpeakerArr::kEmpty;
      case 1:
        return Vst::SpeakerArr::kMono;
      case 2:
        return Vst::SpeakerArr::kStereo;
      case 5:
        return Vst::SpeakerArr::k50;
      case 6:
        return Vst::SpeakerArr::k51;
      case 7:
        return Vst::SpeakerArr::k70Cine;
      case 8:
        return Vst::SpeakerArr::k71Cine;
      default:
        // a SpeakerArrangement can hold at most 64 channels
        if (channel_count > 64)
        {
          return Vst::SpeakerArr::kEmpty;
        }
        if (channel_count == 64)
        {
          return ~Vst::SpeakerArrangement{0};
        }
        // bitmask with channel_count bits set
        return (Vst::SpeakerArrangement{1} << channel_count) - 1;
    }
  }

  if (!strcmp(port_type, CLAP_PORT_MONO)) return Vst::SpeakerArr::kMono;
  if (!strcmp(port_type, CLAP_PORT_STEREO)) return Vst::SpeakerArr::kStereo;

  return Vst::SpeakerArr::kEmpty;
}

void ClapAsVst3::addAudioBusFrom(const clap_audio_port_info_t *info, bool is_input)
{
  auto spk = speakerArrFromPortType(info->port_type, info->channel_count);

  auto bustype = Vst::BusTypes::kMain;  // actually, everything is main, except
  if (is_input && !(info->flags & CLAP_AUDIO_PORT_IS_MAIN))
  {
    // only inputs can be sidechains, everything that is not the MAIN bus is a sidechain
    bustype = Vst::BusTypes::kAux;
  }

  // bool supports64bit = (info->flags & CLAP_AUDIO_PORT_SUPPORTS_64BITS);
  Steinberg::char16 name16[256];
  // str8tostr16 writes to position n to terminate, so don't overflow

  // Steinberg::str8ToStr16(&name16[0], info->name, 255);
  utf8_to_utf16l(info->name, (uint16_t *)name16, 255);
  if (is_input)
  {
    addAudioInput(name16, spk, bustype, Vst::BusInfo::kDefaultActive);
  }
  else
  {
    addAudioOutput(name16, spk, bustype, Vst::BusInfo::kDefaultActive);
  }
}

void ClapAsVst3::addMIDIBusFrom(const clap_note_port_info_t *info, uint32_t index, bool is_input)
{
  if ((info->supported_dialects & CLAP_NOTE_DIALECT_MIDI) ||
      (info->supported_dialects & CLAP_NOTE_DIALECT_CLAP))
  {
    auto numchannels = 16;
    if (_vst3specifics)
    {
      numchannels = _vst3specifics->getNumMIDIChannels(_plugin->_plugin, index);
    }

    Steinberg::char16 name16[256];
    // str8tostr16 writes to position n to terminate, so don't overflow
    // Steinberg::str8ToStr16(&name16[0], info->name, 255);
    utf8_to_utf16l(info->name, (uint16_t *)name16, 255);
    if (is_input)
    {
      addEventInput(name16, numchannels, Vst::BusTypes::kMain, Vst::BusInfo::kDefaultActive);
    }
    else
    {
      addEventOutput(name16, numchannels, Vst::BusTypes::kMain, Vst::BusInfo::kDefaultActive);
    }
  }
}

void ClapAsVst3::updateAudioBusses()
{
  for (auto i = 0U; i < audioInputs.size(); ++i)
  {
    _processAdapter->activateAudioBus(Vst::kInput, i, audioInputs[i]->isActive());
  }
  for (auto i = 0U; i < audioOutputs.size(); ++i)
  {
    _processAdapter->activateAudioBus(Vst::kOutput, i, audioOutputs[i]->isActive());
  }
}

static std::vector<std::string> split(const std::string &s, char delimiter)
{
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(s);
  while (std::getline(tokenStream, token, delimiter))
  {
    tokens.push_back(token);
  }
  return tokens;
}

Vst::UnitID ClapAsVst3::getOrCreateUnitInfo(const char *modulename)
{
  // lookup the modulename fast and return the unitID
  auto loc = _moduleToUnit.find(modulename);
  if (loc != _moduleToUnit.end())
  {
    return loc->second;
  }

  // a leading `/`is wrong
  while (modulename[0] == '/')
  {
    modulename++;
  }

  // the module name is not yet present as unit, so
  // we will ensure that it is being created one by one
  auto path = split(modulename, '/');
  std::string curpath;
  Vst::UnitID id = Vst::kRootUnitId;  // there is already a root element
  size_t i = 0;
  while (path.size() > i)
  {
    loc = _moduleToUnit.find(path[i]);
    if (loc != _moduleToUnit.end())
    {
      if (!curpath.empty())
      {
        curpath.append("/");
      }
      curpath.append(path[i]);
      id = loc->second;
      ++i;
    }
    else
    {
      Steinberg::Vst::String128 name;
      std::string u8name(path[i]);
      if (stringconv::convert(u8name, name))
      {
        auto newid = static_cast<Steinberg::int32>(units.size());
        auto *newunit = new Vst::Unit(name, newid, id);  // a new unit without a program list
        addUnit(newunit);
        _moduleToUnit[u8name] = newid;
        id = newid;
      }
      ++i;
    }
  }
  return id;
}

// Clap::IHost

void ClapAsVst3::setupWrapperSpecifics(const clap_plugin_t *plugin)
{
  _useIMidiMapping = checkMIDIDialectSupport();

  _vst3specifics = (clap_plugin_as_vst3_t *)plugin->get_extension(plugin, CLAP_PLUGIN_AS_VST3);
  if (_vst3specifics)
  {
    _numMidiChannels = _vst3specifics->getNumMIDIChannels(_plugin->_plugin, 0);
    _expressionmap = _vst3specifics->supportedNoteExpressions(_plugin->_plugin);
  }
}

bool ClapAsVst3::checkMIDIDialectSupport()
{
  // check if the plugin supports noteports and if one of the note ports supports MIDI dialect
  if (auto noteports = _plugin->_ext._noteports; noteports)
  {
    auto numMIDIInputs = noteports->count(_plugin->_plugin, true);
    for (uint32_t i = 0; i < numMIDIInputs; ++i)
    {
      clap_note_port_info_t info;
      if (noteports->get(_plugin->_plugin, i, true, &info))
      {
        if (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
        {
          return true;
        }
      }
    }
  }

  return false;
}

void ClapAsVst3::setupAudioBusses(const clap_plugin_t *plugin,
                                  const clap_plugin_audio_ports_t *audioports)
{
  if (!audioports) return;

  // removeAudioBusses() destroys the bus objects and with them the activation
  // states the host has set via activateBus - freshly created busses are
  // always inactive. Remember the states so busses that persist across the
  // rebuild (by index) keep them.
  std::vector<bool> inputsActive, outputsActive;
  inputsActive.reserve(audioInputs.size());
  outputsActive.reserve(audioOutputs.size());
  for (auto i = 0U; i < audioInputs.size(); ++i)
  {
    inputsActive.push_back(audioInputs[i]->isActive());
  }
  for (auto i = 0U; i < audioOutputs.size(); ++i)
  {
    outputsActive.push_back(audioOutputs[i]->isActive());
  }

  removeAudioBusses();

  auto numAudioInputs = audioports->count(plugin, true);
  auto numAudioOutputs = audioports->count(plugin, false);

  fprintf(stderr, "\tAUDIO in: %d, out: %d\n", (int)numAudioInputs, (int)numAudioOutputs);

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

  for (auto i = 0U; i < audioInputs.size() && i < inputsActive.size(); ++i)
  {
    audioInputs[i]->setActive(inputsActive[i]);
  }
  for (auto i = 0U; i < audioOutputs.size() && i < outputsActive.size(); ++i)
  {
    audioOutputs[i]->setActive(outputsActive[i]);
  }
}

void ClapAsVst3::setupMIDIBusses(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *noteports)
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
      addMIDIBusFrom(&info, i, true);
    }
  }
  for (decltype(numMIDIOutPorts) i = 0; i < numMIDIOutPorts; ++i)
  {
    clap_note_port_info_t info;
    if (noteports->get(plugin, i, false, &info))
    {
      addMIDIBusFrom(&info, i, false);
    }
  }
}

void ClapAsVst3::setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params)
{
  if (!params) return;

  // clear the units, they will be rebuild during the parameter conversion
  _moduleToUnit.clear();
  units.clear();

  {
    Vst::UnitInfo rootInfo;
    rootInfo.id = Vst::kRootUnitId;
    rootInfo.parentUnitId = Vst::kNoParentUnitId;
    rootInfo.programListId = Vst::kNoProgramListId;
    stringconv::convert(std::string("Root"), rootInfo.name);

    auto rootUnit = new Vst::Unit(rootInfo);
    addUnit(rootUnit);
  }

  auto numparams = params->count(plugin);
  parameters.removeAll();
  this->parameters.init(numparams);
  for (decltype(numparams) i = 0; i < numparams; ++i)
  {
    clap_param_info info;
    if (params->get_info(plugin, i, &info))
    {
      auto p = Vst3Parameter::create(
          &info, [&](const char *modstring) { return this->getOrCreateUnitInfo(modstring); });
      // auto p = Vst3Parameter::create(&info,nullptr);
      p->param_index_for_clap_get_info = i;
      parameters.addParameter(p);
    }
  }

  if (_useIMidiMapping)
  {
    // find free tags for IMidiMapping
    Vst::ParamID x = 0xb00000;
    _IMidiMappingEasy = true;
    _MIDIUnits.clear();

    for (uint8_t channel = 0; channel < _numMidiChannels; channel++)
    {
      // the unit for that channel
      Vst::UnitInfo midiUnitInfo;

      midiUnitInfo.id = (decltype(midiUnitInfo.id))units.size();
      midiUnitInfo.parentUnitId = 0;  // parented in the root unit
      midiUnitInfo.programListId = Vst::kNoProgramListId;

      auto name = fmt::format("MIDI Channel {}", channel + 1);

      stringconv::convert(name, midiUnitInfo.name);

      for (int i = 0; i < Vst::ControllerNumbers::kCountCtrlNumber; ++i)
      {
        while (parameters.getParameter(x))
        {
          // if this happens there is a index clash between the parameter ids
          // and the ones reserved for the IMidiMapping
          _IMidiMappingEasy = false;
          x++;
        }
        auto p = Vst3Parameter::create(0, channel, i, x);
        p->setUnitID(midiUnitInfo.id);
        parameters.addParameter(p);
        _IMidiMappingIDs[channel][i] = x++;
      }
      // if (false)
      {
        // program change parameter
        while (parameters.getParameter(x))
        {
          // if this happens there is a index clash between the parameter ids
          // and the ones reserved for the IMidiMapping
          _IMidiMappingEasy = false;
          x++;
        }
        auto p = Vst3Parameter::create(0, channel, Vst::ControllerNumbers::kCtrlProgramChange, x);

        p->setUnitID(midiUnitInfo.id);
        _MIDIUnits.emplace_back(midiUnitInfo.id);

        parameters.addParameter(p);

        auto programlist = new Steinberg::Vst::ProgramList(STR16("Program Changes"), x, midiUnitInfo.id);
        for (int pc = 0; pc < 128; ++pc)
        {
          auto programname = fmt::format("Program {}", pc + 1);

          programlist->addProgram(stringconv::convert(programname).c_str());
        }
        this->addProgramList(programlist);

        auto newUnit = new Vst::Unit(midiUnitInfo);

        addUnit(newUnit);

        // the programlist ID is actually the parameter ID
        newUnit->setProgramListID(x);

        //_IMidiMappingIDs[channel][Vst::ControllerNumbers::kCtrlProgramChange] = x++;
        x++;
      }
    }
  }

  // setting up noteexpression

  if (_expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_VOLUME)
    _noteExpressions.addNoteExpressionType(new Vst::NoteExpressionType(
        Vst::NoteExpressionTypeIDs::kVolumeTypeID, S16("Volume"), S16("Vol"), S16(""), 0, nullptr, 0));
  if (_expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_PAN)

    _noteExpressions.addNoteExpressionType(new Vst::NoteExpressionType(
        Vst::NoteExpressionTypeIDs::kPanTypeID, S16("Panorama"), S16("Pan"), S16(""), 0, nullptr, 0));

  if (_expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_TUNING)
    _noteExpressions.addNoteExpressionType(new Vst::NoteExpressionType(
        Vst::NoteExpressionTypeIDs::kTuningTypeID, S16("Tuning"), S16("Tun"), S16(""), 0, nullptr, 0));

  if (_expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_VIBRATO)
    _noteExpressions.addNoteExpressionType(
        new Vst::NoteExpressionType(Vst::NoteExpressionTypeIDs::kVibratoTypeID, S16("Vibrato"),
                                    S16("Vibr"), S16(""), 0, nullptr, 0));

  if (_expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_EXPRESSION)
    _noteExpressions.addNoteExpressionType(
        new Vst::NoteExpressionType(Vst::NoteExpressionTypeIDs::kExpressionTypeID, S16("Expression"),
                                    S16("Expr"), S16(""), 0, nullptr, 0));

  if (_expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_BRIGHTNESS)
    _noteExpressions.addNoteExpressionType(
        new Vst::NoteExpressionType(Vst::NoteExpressionTypeIDs::kBrightnessTypeID, S16("Brightness"),
                                    S16("Brit"), S16(""), 0, nullptr, 0));

  // PRESSURE is handled by IMidiMapping (-> Polypressure)

  setupPresets();
}

// ----------------------------------------------------------------------------
// clap.preset-load, published to the host as a VST3 program list
// ----------------------------------------------------------------------------
void ClapAsVst3::setupPresets()
{
  _presetParamId = Vst::kNoParamId;
  _presetUnitId = Vst::kRootUnitId;

  // setupParameters() can run more than once (param_rescan, and the rebuild in
  // onIdle below), and the index is shared and long-lived - so drop any
  // listener from a previous pass rather than stacking another onto it.
  if (_presetIndex && _presetIndexToken)
  {
    _presetIndex->removeCompletionListener(_presetIndexToken);
    _presetIndexToken = 0;
  }

  // No point offering a list the plugin could not load from.
  if (!_plugin || !_plugin->supportsPresetLoad()) return;

  // Shared per module: the crawl is expensive and its result is identical for
  // every instance of the same plugin.
  if (!_presetIndex)
  {
    const auto *descriptor = _library->plugins[_libraryIndex];
    _presetIndex = Clap::PresetIndex::forPlugin(_library, descriptor->id ? descriptor->id : "");
  }
  if (!_presetIndex) return;  // the plugin has no preset-discovery factory

  // A free tag, away from both the CLAP parameter ids and the block the
  // IMidiMapping parameters reserve at 0xb00000.
  Vst::ParamID id = 0xc00000;
  while (parameters.getParameter(id)) ++id;

  // The list has to be sized now, because a parameter's stepCount is fixed at
  // creation and a host reads stepCount+1 as the program count (the SDK's own
  // preset sample sets kNumPrograms-1). So wait briefly for the crawl rather
  // than announce a size that is wrong: an embedded container resolves in
  // milliseconds, and a folder crawl that outlasts the wait is still covered
  // by the rescan onIdle() asks for when it completes.
  _presetIndex->waitUntilComplete(1000);
  const auto presetCount = _presetIndex->size();
  if (presetCount == 0) return;  // nothing to show; the rescan will come back

  auto *selector = Vst3Parameter::createPresetSelector(id, (int32_t)presetCount);

  // The program list goes on the ROOT unit, and the selector with it. Not a
  // unit of its own: a host reads the root unit's programListId to find "the
  // plugin's programs" - that is what the SDK's mda sample does
  // (mdaBaseController.cpp: uinfo.id = kRootUnitId; uinfo.programListId =
  // kPresetParam) and what againcontroller.cpp means by "create root only if
  // you want to use the programListId". Hung off a child unit instead, the
  // list validates perfectly and Cubase shows nothing.
  selector->setUnitID(Vst::kRootUnitId);
  parameters.addParameter(selector);

  // units[0] is the root unit setupParameters() created just above. Setting
  // the id on the Unit object rather than rebuilding it is how the
  // IMidiMapping block already does it (newUnit->setProgramListID).
  if (!units.empty()) units.at(0)->setProgramListID((Vst::ProgramListID)id);

  _presetParamId = id;
  _presetUnitId = Vst::kRootUnitId;

  // The crawl may already be done - addCompletionListener() calls straight
  // back in that case, which is why _presetParamId is set before this.
  _presetIndexToken = _presetIndex->addCompletionListener([this]() { onPresetIndexComplete(); });
}

void ClapAsVst3::onPresetIndexComplete()
{
  // Called from the index's crawl thread. Nothing that talks to the host may
  // happen here; onIdle() picks this up on the main thread.
  _presetListChanged.store(true);
}

void ClapAsVst3::onRequestPresetLoad(size_t presetIndex)
{
  // Audio thread. Record and return - see the member's comment on coalescing.
  //
  // A value equal to the one already in effect is not a request: the
  // parameter stream carries the selector's value, not its edges, and acting
  // on every arrival makes loading a preset a permanent state of reloading it
  // (\see _presetIndexInEffect).
  if (static_cast<int64_t>(presetIndex) == _presetIndexInEffect.load(std::memory_order_relaxed))
  {
    return;
  }

  _presetLoadRequest.store(static_cast<int64_t>(presetIndex));
}

void ClapAsVst3::preset_loaded(uint32_t locationKind, const char *location, const char *loadKey)
{
  // The plugin loaded a preset - the one onIdle() asked for, or one of its own
  // accord (its own UI, most likely). Move the selector so the host's program
  // display follows, but only if the preset is one this wrapper actually
  // indexed - a plugin can load from places we never crawled, and there is no
  // slot to point at for those.
  if (!_presetIndex || _presetParamId == Vst::kNoParamId) return;

  size_t index = 0;
  if (!_presetIndex->indexOf(locationKind, location, loadKey, index)) return;

  auto *param = (Vst3Parameter *)parameters.getParameter(_presetParamId);
  if (!param) return;

  _presetIndexInEffect.store(static_cast<int64_t>(index), std::memory_order_relaxed);

  const auto normalized = param->asVst3Value(static_cast<double>(index));
  if (param->getNormalized() == normalized)
  {
    // Already where the host put it, which is the usual case: this is the
    // confirmation of a load the host itself asked for. Reporting it as an
    // edit is what a host hands back as a fresh program change.
    return;
  }

  param->setNormalized(normalized);
  if (componentHandler)
  {
    // Bracketed, like any value a plugin originates: an unbracketed
    // performEdit() is a change a host cannot attribute to a gesture, and the
    // ones that record it leave the parameter latched in touch mode.
    componentHandler->beginEdit(_presetParamId);
    componentHandler->performEdit(_presetParamId, normalized);
    componentHandler->endEdit(_presetParamId);
  }
}

void ClapAsVst3::preset_load_error(uint32_t /*locationKind*/, const char * /*location*/,
                                   const char * /*loadKey*/, int32_t /*osError*/, const char *msg)
{
  // VST3 has no channel for this. Log it so it is at least discoverable; the
  // plugin has already been told, and it is the one with a UI to say so in.
  if (_plugin) _plugin->log(CLAP_LOG_WARNING, msg ? msg : "preset load failed");
}

Steinberg::int32 PLUGIN_API ClapAsVst3::getProgramListCount()
{
  return super::getProgramListCount() + (_presetParamId != Vst::kNoParamId ? 1 : 0);
}

Steinberg::tresult PLUGIN_API ClapAsVst3::getProgramListInfo(Steinberg::int32 listIndex,
                                                             Vst::ProgramListInfo &info)
{
  const auto inherited = super::getProgramListCount();
  if (listIndex < inherited) return super::getProgramListInfo(listIndex, info);

  if (_presetParamId == Vst::kNoParamId || listIndex != inherited) return Steinberg::kResultFalse;

  info.id = (Vst::ProgramListID)_presetParamId;
  // What the host will show as the number of slots. Reporting the count found
  // so far (rather than the parameter's 128 steps) keeps a browser from
  // listing empty entries while the crawl is still running.
  info.programCount = _presetIndex ? (Steinberg::int32)_presetIndex->size() : 0;
  stringconv::convert(std::string("Presets"), info.name);
  return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API ClapAsVst3::getProgramName(Vst::ProgramListID listId,
                                                         Steinberg::int32 programIndex,
                                                         Vst::String128 name)
{
  if (!isPresetProgramList(listId)) return super::getProgramName(listId, programIndex, name);

  Clap::PresetEntry entry;
  if (!_presetIndex || programIndex < 0 || !_presetIndex->presetAt((size_t)programIndex, entry))
    return Steinberg::kResultFalse;

  stringconv::convert(entry.displayName(), name);
  return Steinberg::kResultOk;
}

void ClapAsVst3::param_rescan(clap_param_rescan_flags flags)
{
  auto vstflags = 0u;
  if (flags & CLAP_PARAM_RESCAN_ALL)
  {
    setupParameters(_plugin->_plugin, _plugin->_ext._params);
    vstflags |= Vst::RestartFlags::kMidiCCAssignmentChanged;
  }

  vstflags |=
      ((flags & CLAP_PARAM_RESCAN_VALUES) ? (uint32_t)Vst::RestartFlags::kParamValuesChanged : 0u);
  vstflags |= ((flags & CLAP_PARAM_RESCAN_INFO)
                   ? Vst::RestartFlags::kParamValuesChanged | Vst::RestartFlags::kParamTitlesChanged
                   : 0u);

  if (vstflags == 0) return;

  // every path from here on syncs the values, which ::setState relies on -
  // keep this assignment below the early-out above
  _paramValuesSyncedDuringLoad = true;

  // update parameter values in our own tree
  syncParameterValuesFromClap();

  if ((flags & CLAP_PARAM_RESCAN_INFO) && _plugin->_ext._params)
  {
    // In this case, the name and module can also change.
    // For now, don't rebuild the unit tree with modules but
    // do rescan the name
    auto len = parameters.getParameterCount();
    for (decltype(len) i = 0; i < len; ++i)
    {
      auto p = static_cast<Vst3Parameter *>(parameters.getParameterByIndex(i));
      if (p->isMidi) continue;
      clap_param_info_t info;
      if (_plugin->_ext._params->get_info(_plugin->_plugin, p->param_index_for_clap_get_info, &info))
      {
        str8ToStr16(p->getInfo().title, info.name, str16BufferSize(p->getInfo().title));
      }
    }
  }

  // a plugin can request a rescan from within state load(), which a host may
  // call before it hands us the component handler
  if (this->componentHandler) this->componentHandler->restartComponent(vstflags);
}

void ClapAsVst3::param_clear(clap_id param, clap_param_clear_flags flags)
{
  auto vst3id = param & 0x7FFFFFFF;
  // auto* p = (Vst3Parameter*)(parameters.getParameter(param & 0x7FFFFFFF));
  if (flags & CLAP_PARAM_CLEAR_ALL)
  {
    this->parameters.removeParameter(vst3id);
  }
  // all other flags can not be really mapped to VST3 functions
}

// request_flush requests a defered call to flush if there is no processing
void ClapAsVst3::param_request_flush()
{
  _requestedFlush = true;
}

bool ClapAsVst3::gui_can_resize()
{
  // the plugin asks if the host can do a resize
  return (componentHandler2 != nullptr);
}

bool ClapAsVst3::gui_request_resize(uint32_t width, uint32_t height)
{
  // UIs with 65kx65k resolution are not supported
  if ((width > 0xffff) || (height > 0xffff)) return false;

  if (_main_thread_id != std::this_thread::get_id())
  {
    uint32_t newSize = ((width & 0xffff) << 16) | (height & 0xffff);
    _gui_resize_request.store(newSize);
    return true;
  }

  if (_wrappedview)
    return _wrappedview->request_resize(width, height);
  else
    return false;
}

bool ClapAsVst3::gui_request_show()
{
  if (componentHandler2) return (componentHandler2->requestOpenEditor() == kResultOk);
  return false;
}

bool ClapAsVst3::gui_request_hide()
{
  return false;
}

void ClapAsVst3::latency_changed()
{
  if (this->componentHandler)
    this->componentHandler->restartComponent(Vst::RestartFlags::kLatencyChanged);
}

void ClapAsVst3::tail_changed()
{
  // TODO: this could also be kIoChanged, we have to check this
  // a plugin can report a tail change from within state load(), which a host may
  // call before it hands us the component handler - as param_rescan guards for
  if (this->componentHandler)
    this->componentHandler->restartComponent(Vst::RestartFlags::kLatencyChanged);
}

void ClapAsVst3::mark_dirty()
{
  if (componentHandler2) componentHandler2->setDirty(true);
}

void ClapAsVst3::request_process()
{
  // Nothing VST3 can do with this. IComponentHandler has no "start pulling me"
  // and the host alone decides when process() runs, so there is no honest
  // wake here -- and no flush fallback either: onIdle() only runs while the
  // component is active (os::attach lives in setActive), and while it is
  // active the parameter flush belongs to the audio thread.
}

void ClapAsVst3::request_callback()
{
  _requestUICallback = true;
}

void ClapAsVst3::restartPlugin()
{
  _requestRestart = true;
}

void ClapAsVst3::onBeginEdit(clap_id id)
{
  // receive beginEdit and pass it to the internal queue
  _queueToUI.push(beginEvent(id));
}
void ClapAsVst3::onPerformEdit(const clap_event_param_value_t *value)
{
  // receive a value change and pass it to the internal queue
  _queueToUI.push(valueEvent(value));
}
void ClapAsVst3::onEndEdit(clap_id id)
{
  _queueToUI.push(endEvent(id));
}

// track-info
bool ClapAsVst3::track_info_get(clap_track_info_t *info)
{
  if (_trackInfo)
  {
    *info = *_trackInfo;
    return true;
  }

  return false;
}

// ext-timer
bool ClapAsVst3::register_timer(uint32_t period_ms, clap_id *timer_id)
{
  // restrict the timer to 30ms
  if (period_ms < 30)
  {
    period_ms = 30;
  }

  auto l = _timersObjects.size();
  for (decltype(l) i = 0; i < l; ++i)
  {
    auto &to = _timersObjects[i];
    if (to.period == 0)
    {
      // reuse timer object. Lets choose not to use 0 as a timer id, just
      // to make debugging a bit clearer
      to.timer_id = static_cast<clap_id>(i + 1000);
      to.period = period_ms;
      to.nexttick = os::getTickInMS() + period_ms;
      // pass the id to the plugin
      *timer_id = to.timer_id;
#if LIN
      to.handler.reset();
      attachTimers(_iRunLoop);
#endif
      return true;
    }
  }
  // create a new timer object
  auto newid = (clap_id)(l + 1000);
  TimerObject f{period_ms, os::getTickInMS() + period_ms, newid};
  *timer_id = newid;
  _timersObjects.push_back(f);
#if LIN
  attachTimers(_iRunLoop);
#endif

  return true;
}
bool ClapAsVst3::unregister_timer(clap_id timer_id)
{
  for (auto &to : _timersObjects)
  {
    if (to.timer_id == timer_id)
    {
      to.period = 0;
      to.nexttick = 0;
#if LIN
      if (to.handler && _iRunLoop)
      {
        _iRunLoop->unregisterTimer(to.handler.get());
      }
      to.handler.reset();
#endif
      return true;
    }
  }
  return false;
}

const char *ClapAsVst3::host_get_name()
{
  if (vst3HostApplication)
  {
    Steinberg::Vst::String128 res;
    if (kResultOk == vst3HostApplication->getName(res))
    {
      wrapper_hostname = stringconv::convert(res);
      wrapper_hostname.append(" (CLAP-as-VST3)");
    }
  }
  return wrapper_hostname.c_str();
}

void ClapAsVst3::onIdle()
{
  if (!_plugin || !_plugin->_plugin) return;

  // On the run loop path this is the host's own main thread and the lock is
  // uncontended. On the Linux helper thread it is not, so take it with
  // try_lock: the helper holds a module-wide lock while it idles, and blocking
  // here would stall every other instance behind whichever one the host is
  // currently inside. Anything skipped is picked up on the next tick.
  std::unique_lock<std::recursive_mutex> mainThreadGuard(_mainThreadLock, std::try_to_lock);
  if (!mainThreadGuard.owns_lock()) return;

  // Makes clap_host_thread_check::is_main_thread() answer true for the
  // duration. The lock above is what earns that answer rather than merely
  // asserting it: while it is held, the thread the host calls the main thread
  // cannot be in here as well.
  auto mainThreadOverride = _plugin->AlwaysMainThread();

  // A preset the host asked for on the audio thread, and a preset list that
  // filled in on the crawl thread. Both have to happen here: from_location()
  // is [main-thread], and so is notifyProgramListChange().
  if (auto requested = _presetLoadRequest.exchange(-1); requested >= 0)
  {
    Clap::PresetEntry entry;
    // Clamp anyway: stepCount matches the count at creation, but a host may
    // still have a stale value from before a rescan.
    const auto count = _presetIndex ? _presetIndex->size() : 0;
    if (count > 0)
    {
      const auto index = std::min<size_t>((size_t)requested, count - 1);

      // In effect from here on, whatever the load makes of it: the host is
      // sending this value, and a preset that cannot be loaded has to be
      // attempted once rather than once per block. A load that succeeds
      // confirms the same index through preset_loaded().
      _presetIndexInEffect.store(static_cast<int64_t>(index), std::memory_order_relaxed);

      if (_presetIndex->presetAt(index, entry))
      {
        _plugin->loadPresetFromLocation(entry.locationKind,
                                        entry.location.empty() ? nullptr : entry.location.c_str(),
                                        entry.loadKey.empty() ? nullptr : entry.loadKey.c_str());
      }
    }
  }

  if (_presetListChanged.exchange(false))
  {
    if (_presetParamId == Vst::kNoParamId)
    {
      // The crawl finished after setupPresets() had nothing to size the list
      // with, so there is no selector parameter at all yet. Only rebuilding
      // the parameters can introduce one; restartComponent is what makes the
      // host re-read them.
      setupParameters(_plugin->_plugin, _plugin->_ext._params);
      if (componentHandler)
        componentHandler->restartComponent(Vst::RestartFlags::kParamTitlesChanged |
                                           Vst::RestartFlags::kParamValuesChanged);
    }
    else if (auto unitHandler = Steinberg::FUnknownPtr<Vst::IUnitHandler>(componentHandler))
    {
      // -1: every program in the list changed, not one of them.
      unitHandler->notifyProgramListChange((Vst::ProgramListID)_presetParamId, -1);
    }
  }

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
        auto param = (Vst3Parameter *)(parameters.getParameter(n._data._value.param_id & 0x7FFFFFFF));
        auto v = n._data._value.value;
        performEdit(param->getInfo().id, param->asVst3Value(v));
      }
      break;
      case queueEvent::type_t::editend:
        endEdit(n._data._id);
        break;
    }
  }

  if (_requestedFlush)
  {
    // Lock against ::process with a spin lock
    ClapWrapper::detail::shared::SpinLockGuard everLock(_processOrFlushLock);
    // Lock against setProcess with a mutex
    std::lock_guard lock(_processingLock);

    _requestedFlush = false;
    if (!_processing || !_processEverCalled)
    {
      // setup a ProcessAdapter just for flush with no audio
      Clap::ProcessAdapter pa;
      pa.setupProcessing(_plugin->_plugin, _plugin->_ext._params, audioInputs, audioOutputs, 0, 0, 0,
                         this->parameters, componentHandler, this, _gesturedparameters, false, false);
      auto thisFn = _plugin->AlwaysAudioThread();  // just to pacify the clap-helper

      pa.flush();
    }
  }

  if (_requestUICallback)
  {
    _requestUICallback = false;
    _plugin->_plugin->on_main_thread(_plugin->_plugin);
  }

  if (_requestRestart)
  {
    _requestRestart = false;
    if (componentHandler)
      componentHandler->restartComponent(Vst::RestartFlags::kIoChanged |
                                         Vst::RestartFlags::kLatencyChanged);
  }

  if (_wrappedview)
  {
    if (auto const size = _gui_resize_request.exchange(_gui_invalid_size); size != _gui_invalid_size)
    {
      auto w = (size >> 16) & 0xffff;
      auto h = (size & 0xffff);
      _wrappedview->request_resize(w, h);
    }
  }

#if LIN
  // With a run loop the timers are registered on it directly and fire as
  // ITimerHandlers. Without one this idle belongs to the helper thread, and
  // driving them here is what makes clap timers work with no editor open.
  if (!_iRunLoop)
#endif
  {
    // handling timerobjects
    auto now = os::getTickInMS();
    for (auto &&to : _timersObjects)
    {
      if (to.period > 0 && to.nexttick < now)
      {
        to.nexttick = now + to.period;
        this->_plugin->_ext._timer->on_timer(_plugin->_plugin, to.timer_id);
      }
    }
  }
}

#if LIN
struct TimerHandler : Steinberg::Linux::ITimerHandler, public Steinberg::FObject
{
  ClapAsVst3 *_parent{nullptr};
  clap_id _timerId{0};
  TimerHandler(ClapAsVst3 *parent, clap_id timerId) : _parent(parent), _timerId(timerId)
  {
  }
  void PLUGIN_API onTimer() final
  {
    _parent->fireTimer(_timerId);
  }
  DELEGATE_REFCOUNT(Steinberg::FObject)
  DEFINE_INTERFACES
  DEF_INTERFACE(Steinberg::Linux::ITimerHandler)
  END_DEFINE_INTERFACES(Steinberg::FObject)
};

struct IdleHandler : Steinberg::Linux::ITimerHandler, public Steinberg::FObject
{
  ClapAsVst3 *_parent{nullptr};
  IdleHandler(ClapAsVst3 *parent) : _parent(parent)
  {
  }
  void PLUGIN_API onTimer() final
  {
    _parent->onIdle();
  }
  DELEGATE_REFCOUNT(Steinberg::FObject)
  DEFINE_INTERFACES
  DEF_INTERFACE(Steinberg::Linux::ITimerHandler)
  END_DEFINE_INTERFACES(Steinberg::FObject)
};

void ClapAsVst3::attachTimers(Steinberg::Linux::IRunLoop *r)
{
  if (r)
  {
    _iRunLoop = r;

    if (_idleHandler)
    {
      _iRunLoop->unregisterTimer(_idleHandler.get());
    }
    else
    {
      _idleHandler = Steinberg::owned(new IdleHandler(this));
    }
    _iRunLoop->registerTimer(_idleHandler.get(), 30);

    for (auto &t : _timersObjects)
    {
      if (!t.handler)
      {
        t.handler = Steinberg::owned(new TimerHandler(this, t.timer_id));
        _iRunLoop->registerTimer(t.handler.get(), t.period);
      }
    }

    // the host's own main thread drives the idle from here on
    os::idleSourceChanged();
  }
}

void ClapAsVst3::detachTimers(Steinberg::Linux::IRunLoop *r)
{
  if (r && r == _iRunLoop)
  {
    if (_idleHandler)
    {
      _iRunLoop->unregisterTimer(_idleHandler.get());
      _idleHandler.reset();
    }
    for (auto &t : _timersObjects)
    {
      if (t.handler)
      {
        _iRunLoop->unregisterTimer(t.handler.get());
        t.handler.reset();
      }
    }
  }
}

void ClapAsVst3::fireTimer(clap_id timer_id)
{
  _plugin->_ext._timer->on_timer(_plugin->_plugin, timer_id);
}

bool ClapAsVst3::register_fd(int fd, clap_posix_fd_flags_t flags)
{
  _posixFDObjects.emplace_back(fd, flags);
  attachPosixFD(_iRunLoop);
  return true;
}
bool ClapAsVst3::modify_fd(int fd, clap_posix_fd_flags_t flags)
{
  bool res{false};
  for (auto &p : _posixFDObjects)
  {
    if (p.fd == fd)
    {
      p.flags = flags;
      res = true;
    }
  }
  return res;
}

bool ClapAsVst3::unregister_fd(int fd)
{
  bool res{false};
  auto it = _posixFDObjects.begin();
  while (it != _posixFDObjects.end())
  {
    if (it->fd == fd)
    {
      res = true;
      if (_iRunLoop && it->handler)
      {
        _iRunLoop->unregisterEventHandler(it->handler.get());
      }
      it->handler.reset();
      it = _posixFDObjects.erase(it);
    }
    else
    {
      ++it;
    }
  }
  return res;
}

struct FDHandler : Steinberg::Linux::IEventHandler, public Steinberg::FObject
{
  ClapAsVst3 *_parent{nullptr};
  int _fd{0};
  clap_posix_fd_flags_t _flags{};
  FDHandler(ClapAsVst3 *parent, int fd, clap_posix_fd_flags_t flags)
    : _parent(parent), _fd(fd), _flags(flags)
  {
  }
  void PLUGIN_API onFDIsSet(Steinberg::Linux::FileDescriptor) override
  {
    _parent->firePosixFDIsSet(_fd, _flags);
  }
  DELEGATE_REFCOUNT(Steinberg::FObject)
  DEFINE_INTERFACES
  DEF_INTERFACE(Steinberg::Linux::IEventHandler)
  END_DEFINE_INTERFACES(Steinberg::FObject)
};
void ClapAsVst3::attachPosixFD(Steinberg::Linux::IRunLoop *r)
{
  if (r)
  {
    _iRunLoop = r;

    for (auto &p : _posixFDObjects)
    {
      if (!p.handler)
      {
        p.handler = Steinberg::owned(new FDHandler(this, p.fd, p.flags));
        _iRunLoop->registerEventHandler(p.handler.get(), p.fd);
      }
    }
  }
}

void ClapAsVst3::detachPosixFD(Steinberg::Linux::IRunLoop *r)
{
  if (r && r == _iRunLoop)
  {
    for (auto &p : _posixFDObjects)
    {
      if (p.handler)
      {
        _iRunLoop->unregisterEventHandler(p.handler.get());
        p.handler.reset();
      }
    }
  }
}

void ClapAsVst3::firePosixFDIsSet(int fd, clap_posix_fd_flags_t flags)
{
  _plugin->_ext._posixfd->on_fd(_plugin->_plugin, fd, flags);
}
#endif

void wrapper_context_menu_item::vst3_to_clap(clap_id action_id)
{
  name = std::make_unique<std::string>();
  *name = stringconv::convert(vst3item.name);

  if (vst3item.flags == vst3item.kIsGroupStart)
  {
    kind = CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU;
    this->clap.submenu.label = name->c_str();
    this->clap.submenu.is_enabled = true;  // this works different to the VST3 submenus
    return;
  }
  if (vst3item.flags == vst3item.kIsGroupEnd)
  {
    this->kind = CLAP_CONTEXT_MENU_ITEM_END_SUBMENU;
    return;
  }
  if (vst3item.flags & vst3item.kIsSeparator)
  {
    this->kind = CLAP_CONTEXT_MENU_ITEM_SEPARATOR;
    return;
  }
  // now this is a weird one in VST3 (or in CLAP, depends on the POV)
  if (vst3item.flags & vst3item.kIsChecked)
  {
    this->kind = CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY;
    this->clap.menu_check_entry.label = name->c_str();
    this->clap.menu_check_entry.is_checked = true;  // of course it is checked
    this->clap.menu_check_entry.is_enabled = !(vst3item.flags & vst3item.kIsDisabled);
    this->clap.menu_check_entry.action_id = action_id;
    return;
  }
  this->kind = CLAP_CONTEXT_MENU_ITEM_ENTRY;
  this->clap.entry.label = name->c_str();
  this->clap.entry.is_enabled = !(vst3item.flags & vst3item.kIsDisabled);
  this->clap.entry.action_id = action_id;
}

bool ClapAsVst3::supportsContextMenu() const
{
  return (this->componentHandler3 != nullptr);
}

bool ClapAsVst3::context_menu_populate(const clap_context_menu_target_t *target,
                                       const clap_context_menu_builder_t *builder)
{
  vst3ContextMenu.reset();

  // first check if all entry types are supported.
  if (!builder->supports(builder, CLAP_CONTEXT_MENU_ITEM_ENTRY)) return false;
  if (!builder->supports(builder, CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY)) return false;
  if (!builder->supports(builder, CLAP_CONTEXT_MENU_ITEM_SEPARATOR)) return false;
  if (!builder->supports(builder, CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU)) return false;
  if (!builder->supports(builder, CLAP_CONTEXT_MENU_ITEM_END_SUBMENU)) return false;
  // CLAP_CONTEXT_MENU_ITEM_TITLE is not used by VST3

  if (target->kind == CLAP_CONTEXT_MENU_TARGET_KIND_GLOBAL)
  {
    this->vst3ContextMenu = componentHandler3->createContextMenu(this->_wrappedview, nullptr);
  }
  if (target->kind == CLAP_CONTEXT_MENU_TARGET_KIND_PARAM)
  {
    vst3ContextMenuParamID = target->id;
    vst3ContextMenu = componentHandler3->createContextMenu(_wrappedview, &vst3ContextMenuParamID);
  }
  if (vst3ContextMenu)
  {
    vst3ContextMenu->release();  // the IPtr holds the reference
    // preparing the internal mapping structure with wrapper_context_menu_item
    auto itmcnt = vst3ContextMenu->getItemCount();
    this->contextmenuitems.resize(itmcnt);

    for (decltype(itmcnt) i = 0; i < itmcnt; ++i)
    {
      wrapper_context_menu_item &item = contextmenuitems.at(i);

      if (kResultOk == vst3ContextMenu->getItem(i, item.vst3item, &item.vst3target))
      {
        // create the appropriate clap structure
        item.vst3_to_clap((clap_id)i);

        switch (item.kind)
        {
          case CLAP_CONTEXT_MENU_ITEM_ENTRY:
            builder->add_item(builder, item.kind, &item.clap.entry);
            break;
          case CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY:
            builder->add_item(builder, item.kind, &item.clap.menu_check_entry);
            break;
          case CLAP_CONTEXT_MENU_ITEM_SEPARATOR:
            builder->add_item(builder, item.kind, nullptr);
            break;
          case CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU:
            builder->add_item(builder, item.kind, &item.clap.submenu);
            break;
          case CLAP_CONTEXT_MENU_ITEM_END_SUBMENU:
            builder->add_item(builder, item.kind, nullptr);
            break;
          case CLAP_CONTEXT_MENU_ITEM_TITLE:
            // this does not exist
            break;
          default:
            break;
        }
      }
    }
    return true;
  }

  return false;
}

bool ClapAsVst3::context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id)
{
  (void)target;
  if (action_id < contextmenuitems.size())
  {
    auto &item = contextmenuitems.at(action_id);
    bool okay = (kResultOk == item.vst3target->executeMenuItem(item.vst3item.tag));
    clearContextMenu();
    return okay;
  }
  return false;
}

bool ClapAsVst3::context_menu_can_popup()
{
  return false;
}

bool ClapAsVst3::context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index,
                                    int32_t x, int32_t y)
{
  return false;
}

void ClapAsVst3::clearContextMenu()
{
  vst3ContextMenu.reset();
  contextmenuitems.clear();
}

tresult ClapAsVst3::getBusInfo(Vst::MediaType type, Vst::BusDirection dir, int32 index,
                               Vst::BusInfo &bus)
{
  if (_plugin->_ext._audioports)
  {
    if (type == Vst::kAudio)
    {
      auto raise = _plugin->AlwaysMainThread();

      clap_audio_port_info_t info;
      if (_plugin->_ext._audioports->get(_plugin->_plugin, (uint32_t)index, (dir == Vst::kInput), &info))
      {
        bus.mediaType = Vst::kAudio;
        bus.channelCount = info.channel_count;
        bus.direction = dir;
        bus.flags = Vst::BusInfo::kDefaultActive;

        if (dir == Vst::BusDirections::kOutput)
        {
          bus.busType = Vst::kMain;  // outputs are always main
        }
        else
        {
          bus.busType = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) ? Vst::kMain : Vst::kAux;
        }

        utf8_to_utf16l(info.name, (uint16_t *)&bus.name[0], str16BufferSize(Steinberg::Vst::String128));

        return kResultOk;
      }
      else
      {
        return kResultFalse;
      }
    }
  }
  return SingleComponentEffect::getBusInfo(type, dir, index, bus);
}

double PLUGIN_API ClapAsVst3::getGainReductionValueInDb()
{
  auto *gr = _plugin->_ext._gainreduc;
  if (gr)
  {
    return gr->get(_plugin->_plugin);
  }
  return 0.0;
}
