#include "wrapasvst3.h"
#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ustring.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <public.sdk/source/vst/utility/stringconvert.h>
#include <base/source/fstring.h>
#include "detail/vst3/state.h"
#include "detail/vst3/process.h"
#include "detail/vst3/parameter.h"
#include "detail/clap/fsutil.h"
#include <locale>
#include <sstream>

#if WIN
#include <tchar.h>
#define S16(x) reinterpret_cast<const Steinberg::Vst::TChar*>(_T(x))
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

void utf8_to_utf16l(const char* utf8string, uint16_t* target, size_t targetsize)
{
  uint32_t codepoint = 0;
  size_t targetpos = 0;

  auto src = reinterpret_cast<const uint8_t*>(utf8string);
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
      if (((byte & 0b11100000) == 0b11000000) && src[1])
      {
        codepoint = byte & 0b00011111;
        codepoint = (codepoint << 6) | ((src[pos + 1]) & 0b00111111);
        pos += 2;
      }
      else if (((byte & 0b11110000) == 0b11100000) && src[1] && src[2])
      {
        codepoint = byte & 0b00001111;
        codepoint = (codepoint << 6) | ((src[pos + 1] & 0b00111111));
        codepoint = (codepoint << 6) | ((src[pos + 2] & 0b00111111));
        pos += 3;
      }
      else if (((byte & 0b11111000) == 0b11110000) && src[1] && src[2] && src[3])
      {
        codepoint = byte & 0b00000111;
        codepoint = (codepoint << 6) | ((src[pos + 1] & 0b00111111));
        codepoint = (codepoint << 6) | ((src[pos + 2] & 0b00111111));
        codepoint = (codepoint << 6) | ((src[pos + 3] & 0b00111111));
        pos += 4;
      }
      else
      {
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

tresult PLUGIN_API ClapAsVst3::initialize(FUnknown* context)
{
  auto result = super::initialize(context);
  context->queryInterface(Vst::IHostApplication::iid, (void**)&vst3HostApplication);
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
    if (!_plugin->activate()) return kResultFalse;
    _active = true;
    _processAdapter = new Clap::ProcessAdapter();

    auto supportsnoteexpression =
        (_expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_PRESSURE);

    // the processAdapter needs to know a few things to intercommunicate between VST3 host and CLAP plugin.

    _processAdapter->setupProcessing(
        _plugin->_plugin, _plugin->_ext._params, this->audioInputs, this->audioOutputs,
        this->_largestBlocksize, this->eventInputs.size(), this->eventOutputs.size(), parameters,
        componentHandler, this, supportsnoteexpression,
        _expressionmap & clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_TUNING);
    updateAudioBusses();

    if (_missedLatencyRequest)
    {
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
  }
  return super::setActive(state);
}

tresult PLUGIN_API ClapAsVst3::process(Vst::ProcessData& data)
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

tresult PLUGIN_API ClapAsVst3::setState(IBStream* state)
{
  return (_plugin->load(CLAPVST3StreamAdapter(state)) ? Steinberg::kResultOk : Steinberg::kResultFalse);
}

tresult PLUGIN_API ClapAsVst3::getState(IBStream* state)
{
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

tresult PLUGIN_API ClapAsVst3::setupProcessing(Vst::ProcessSetup& newSetup)
{
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

tresult PLUGIN_API ClapAsVst3::setBusArrangements(Vst::SpeakerArrangement* inputs, int32 numIns,
                                                  Vst::SpeakerArrangement* outputs, int32 numOuts)
{
  if (!_plugin->_ext._audioports)
  {
    return kResultFalse;
  }

  int32_t inc = _plugin->_ext._audioports->count(_plugin->_plugin, true);
  int32_t ouc = _plugin->_ext._audioports->count(_plugin->_plugin, false);
  if (inc != numIns || ouc != numOuts)
  {
    return kResultFalse;
  }

  for (int i = 0; i < numIns; ++i)
  {
    clap_audio_port_info_t info;
    _plugin->_ext._audioports->get(_plugin->_plugin, i, true, &info);
    Vst::SpeakerArrangement sa{0};
    for (auto c = 0U; c < info.channel_count; ++c)
    {
      sa = (sa << 1) + 1;
    }
    if (inputs[i] != sa)
    {
      return kResultFalse;
    }
  }

  for (int i = 0; i < numOuts; ++i)
  {
    clap_audio_port_info_t info;
    _plugin->_ext._audioports->get(_plugin->_plugin, i, false, &info);
    Vst::SpeakerArrangement sa{0};
    for (auto c = 0U; c < info.channel_count; ++c)
    {
      sa = (sa << 1) + 1;
    }
    if (outputs[i] != sa)
    {
      return kResultFalse;
    }
  }

  return super::setBusArrangements(inputs, numIns, outputs, numOuts);
}

tresult PLUGIN_API ClapAsVst3::getBusArrangement(Vst::BusDirection dir, int32 index,
                                                 Vst::SpeakerArrangement& arr)
{
  return super::getBusArrangement(dir, index, arr);
}

IPlugView* PLUGIN_API ClapAsVst3::createView(FIDString /*name*/)
{
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
#endif

              clearContextMenu();
            }
            this->_wrappedview = nullptr;
          },
          [this]()
          {

#if LIN
            attachTimers(_wrappedview->getRunLoop());
            attachPosixFD(_wrappedview->getRunLoop());
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
  auto param = (Vst3Parameter*)this->getParameterObject(id);
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

  if (this->_plugin->_ext._params->value_to_text(_plugin->_plugin, param->id, val, outbuf, 127))
  {
    utf8_to_utf16l(outbuf, (uint16_t*)&string[0], str16BufferSize(Steinberg::Vst::String128));

    return kResultOk;
  }
  return super::getParamStringByValue(id, valueNormalized, string);
}

tresult PLUGIN_API ClapAsVst3::getParamValueByString(Vst::ParamID id, Vst::TChar* string,
                                                     Vst::ParamValue& valueNormalized)
{
  auto param = (Vst3Parameter*)this->getParameterObject(id);
  Steinberg::String m(string);
  char inbuf[128];
  m.copyTo8(inbuf, 0, 128);
  double out = 0.;
  if (param->isMidi)
  {
    return Steinberg::kResultFalse;
  }
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

tresult PLUGIN_API ClapAsVst3::setComponentHandler(Vst::IComponentHandler* handler)
{
  componentHandler3.reset();

  // the base class extracts IComponentHandler and IComponentHandler2
  auto result = super::setComponentHandler(handler);
  // but for context menus IComponentHandler3 is needed, so it is being retrieved here.
  if (componentHandler && result == kResultOk)
  {
    this->componentHandler->queryInterface(Vst::IComponentHandler3::iid, (void**)&componentHandler3);
  }

  return result;
}

//-----------------------------------------------------------------------------

tresult PLUGIN_API ClapAsVst3::getMidiControllerAssignment(int32 busIndex, int16 channel,
                                                           Vst::CtrlNumber midiControllerNumber,
                                                           Vst::ParamID& id /*out*/)
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
                                          Vst::NoteExpressionTypeInfo& info /*out*/)
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
                                                   const Vst::TChar* string /*in*/,
                                                   Vst::NoteExpressionValue& valueNormalized /*out*/)
{
  return _noteExpressions.getNoteExpressionValueByString(id, string, valueNormalized);
}

#endif

tresult ClapAsVst3::getUnitByBus(Vst::MediaType type, Vst::BusDirection dir, int32 busIndex,
                                 int32 channel, Vst::UnitID& unitId /*out*/)
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

static Vst::SpeakerArrangement speakerArrFromPortType(const char* port_type)
{
  if (!port_type) return Vst::SpeakerArr::kEmpty;
  static const std::pair<const char*, Vst::SpeakerArrangement> arrangementmap[] = {
      {CLAP_PORT_MONO, Vst::SpeakerArr::kMono},
      {CLAP_PORT_STEREO, Vst::SpeakerArr::kStereo},
      // {CLAP_PORT_AMBISONIC, Vst::SpeakerArr::kAmbi1stOrderACN} <- we need also CLAP_EXT_AMBISONIC
      // {CLAP_PORT_SURROUND, Vst::SpeakerArr::kStereoSurround}, // add when CLAP_EXT_SURROUND is not draft anymore
      // TODO: add more PortTypes to Speaker Arrangement
      {nullptr, Vst::SpeakerArr::kEmpty}};

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
  // str8tostr16 writes to position n to terminate, so don't overflow

  // Steinberg::str8ToStr16(&name16[0], info->name, 255);
  utf8_to_utf16l(info->name, (uint16_t*)name16, 255);
  if (is_input)
  {
    addAudioInput(name16, spk, bustype, Vst::BusInfo::kDefaultActive);
  }
  else
  {
    addAudioOutput(name16, spk, bustype, Vst::BusInfo::kDefaultActive);
  }
}

void ClapAsVst3::addMIDIBusFrom(const clap_note_port_info_t* info, uint32_t index, bool is_input)
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
    utf8_to_utf16l(info->name, (uint16_t*)name16, 255);
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

static std::vector<std::string> split(const std::string& s, char delimiter)
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

Vst::UnitID ClapAsVst3::getOrCreateUnitInfo(const char* modulename)
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
      if (VST3::StringConvert::convert(u8name, name))
      {
        auto newid = static_cast<Steinberg::int32>(units.size());
        auto* newunit = new Vst::Unit(name, newid, id);  // a new unit without a program list
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

void ClapAsVst3::setupWrapperSpecifics(const clap_plugin_t* plugin)
{
  _useIMidiMapping = checkMIDIDialectSupport();

  _vst3specifics = (clap_plugin_as_vst3_t*)plugin->get_extension(plugin, CLAP_PLUGIN_AS_VST3);
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

void ClapAsVst3::setupAudioBusses(const clap_plugin_t* plugin,
                                  const clap_plugin_audio_ports_t* audioports)
{
  if (!audioports) return;
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

void ClapAsVst3::setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params)
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
    VST3::StringConvert::convert(std::string("Root"), rootInfo.name);

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
          &info, [&](const char* modstring) { return this->getOrCreateUnitInfo(modstring); });
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

      VST3::StringConvert::convert(name, midiUnitInfo.name);

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

          programlist->addProgram(VST3::StringConvert::convert(programname).c_str());
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

  // update parameter values in our own tree
  auto len = parameters.getParameterCount();
  for (decltype(len) i = 0; i < len; ++i)
  {
    auto p = static_cast<Vst3Parameter*>(parameters.getParameterByIndex(i));
    if (!p->isMidi)
    {
      double val;
      if (_plugin->_ext._params->get_value(_plugin->_plugin, p->id, &val))
      {
        auto newval = p->asVst3Value(val);
        if (p->getNormalized() != newval)
        {
          p->setNormalized(newval);
        }
      }
      if (flags & CLAP_PARAM_RESCAN_INFO)
      {
        // In this case, the name and module can also change.
        // For now, don't rebuild the unit tree with modules but
        // do rescan the name
        clap_param_info_t info;
        if (_plugin->_ext._params->get_info(_plugin->_plugin, p->param_index_for_clap_get_info, &info))
        {
          str8ToStr16(p->getInfo().title, info.name, str16BufferSize(p->getInfo().title));
        }
      }
    }
  }

  this->componentHandler->restartComponent(vstflags);
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
  this->componentHandler->restartComponent(Vst::RestartFlags::kLatencyChanged);
}

void ClapAsVst3::mark_dirty()
{
  if (componentHandler2) componentHandler2->setDirty(true);
}

void ClapAsVst3::request_callback()
{
  _requestUICallback = true;
}

void ClapAsVst3::restartPlugin()
{
  if (componentHandler)
    componentHandler->restartComponent(Vst::RestartFlags::kIoChanged |
                                       Vst::RestartFlags::kLatencyChanged);
}

void ClapAsVst3::onBeginEdit(clap_id id)
{
  // receive beginEdit and pass it to the internal queue
  _queueToUI.push(beginEvent(id));
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

// ext-timer
bool ClapAsVst3::register_timer(uint32_t period_ms, clap_id* timer_id)
{
  // restrict the timer to 30ms
  if (period_ms < 30)
  {
    period_ms = 30;
  }

  auto l = _timersObjects.size();
  for (decltype(l) i = 0; i < l; ++i)
  {
    auto& to = _timersObjects[i];
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
  for (auto& to : _timersObjects)
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

const char* ClapAsVst3::host_get_name()
{
  if (vst3HostApplication)
  {
    Steinberg::Vst::String128 res;
    if (kResultOk == vst3HostApplication->getName(res))
    {
      wrapper_hostname = VST3::StringConvert::convert(res);
      wrapper_hostname.append(" (CLAP-as-VST3)");
    }
  }
  return wrapper_hostname.c_str();
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
        auto param = (Vst3Parameter*)(parameters.getParameter(n._data._value.param_id & 0x7FFFFFFF));
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
                         this->parameters, componentHandler, nullptr, false, false);
      auto thisFn = _plugin->AlwaysAudioThread();  // just to pacify the clap-helper

      pa.flush();
    }
  }

  if (_requestUICallback)
  {
    _requestUICallback = false;
    _plugin->_plugin->on_main_thread(_plugin->_plugin);
  }

#if LIN
  if (!_iRunLoop)  // don't process timers if we have a runloop.
                   // (but if we don't have a runloop on linux onIdle isn't called
                   // anyway so consider just not having this at all once we decide
                   // to do with the no UI case)
#endif
  {
    // handling timerobjects
    auto now = os::getTickInMS();
    for (auto&& to : _timersObjects)
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
  ClapAsVst3* _parent{nullptr};
  clap_id _timerId{0};
  TimerHandler(ClapAsVst3* parent, clap_id timerId) : _parent(parent), _timerId(timerId)
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
  ClapAsVst3* _parent{nullptr};
  IdleHandler(ClapAsVst3* parent) : _parent(parent)
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

void ClapAsVst3::attachTimers(Steinberg::Linux::IRunLoop* r)
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

    for (auto& t : _timersObjects)
    {
      if (!t.handler)
      {
        t.handler = Steinberg::owned(new TimerHandler(this, t.timer_id));
        _iRunLoop->registerTimer(t.handler.get(), t.period);
      }
    }
  }
}

void ClapAsVst3::detachTimers(Steinberg::Linux::IRunLoop* r)
{
  if (r && r == _iRunLoop)
  {
    if (_idleHandler)
    {
      _iRunLoop->unregisterTimer(_idleHandler.get());
      _idleHandler.reset();
    }
    for (auto& t : _timersObjects)
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
  for (auto& p : _posixFDObjects)
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
  ClapAsVst3* _parent{nullptr};
  int _fd{0};
  clap_posix_fd_flags_t _flags{};
  FDHandler(ClapAsVst3* parent, int fd, clap_posix_fd_flags_t flags)
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
void ClapAsVst3::attachPosixFD(Steinberg::Linux::IRunLoop* r)
{
  if (r)
  {
    _iRunLoop = r;

    for (auto& p : _posixFDObjects)
    {
      if (!p.handler)
      {
        p.handler = Steinberg::owned(new FDHandler(this, p.fd, p.flags));
        _iRunLoop->registerEventHandler(p.handler.get(), p.fd);
      }
    }
  }
}

void ClapAsVst3::detachPosixFD(Steinberg::Linux::IRunLoop* r)
{
  if (r && r == _iRunLoop)
  {
    for (auto& p : _posixFDObjects)
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
  *name = VST3::StringConvert::convert(vst3item.name);

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

bool ClapAsVst3::context_menu_populate(const clap_context_menu_target_t* target,
                                       const clap_context_menu_builder_t* builder)
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
      wrapper_context_menu_item& item = contextmenuitems.at(i);

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

bool ClapAsVst3::context_menu_perform(const clap_context_menu_target_t* target, clap_id action_id)
{
  (void)target;
  if (action_id < contextmenuitems.size())
  {
    auto& item = contextmenuitems.at(action_id);
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

bool ClapAsVst3::context_menu_popup(const clap_context_menu_target_t* target, int32_t screen_index,
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
                               Vst::BusInfo& bus)
{
  if (_plugin->_ext._audioports)
  {
    if (type == Vst::kAudio)
    {
      clap_audio_port_info_t info;
      if (_plugin->_ext._audioports->get(_plugin->_plugin, (uint32_t)index, (dir == Vst::kInput), &info))
      {
        bus.mediaType = Vst::kAudio;
        bus.channelCount = info.channel_count;
        bus.direction = dir;
        bus.busType = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) ? Vst::kMain : Vst::kAux;
        bus.flags = Vst::BusInfo::kDefaultActive;

        utf8_to_utf16l(info.name, (uint16_t*)&bus.name[0], str16BufferSize(Steinberg::Vst::String128));

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
  auto* gr = _plugin->_ext._gainreduc;
  if (gr)
  {
    return gr->get(_plugin->_plugin);
  }
  return 0.0;
}
