#pragma once

/*
                CLAP as VST3

    Copyright (c) 2022 Timo Kaluza (defiantnerd)

                This file is part of the clap-wrappers project which is released under MIT License.
                See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

                This VST3 opens a CLAP plugin and matches all corresponding VST3 calls to it.
                For the VST3 Host it is a VST3 plugin, for the CLAP plugin it is a CLAP host.

*/

#include "clap_proxy.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstnoteexpression.h>
#include <public.sdk/source/vst/vstsinglecomponenteffect.h>
#include <public.sdk/source/vst/vstnoteexpressiontypes.h>

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "detail/vst3/plugview.h"
#include "detail/vst3/os/osutil.h"
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
  beginEvent(clap_id id) : queueEvent()
  {
    this->_type = type::editstart;
    _data._id = id;
  }
};

class endEvent : public queueEvent
{
public:
  endEvent(clap_id id) : queueEvent()
  {
    this->_type = type::editend;
    _data._id = id;
  }
};

class valueEvent : public queueEvent
{
public:
  valueEvent(const clap_event_param_value_t *value) : queueEvent()
  {
    _type = type::editvalue;
    _data._value = *value;
  }
};

class ClapAsVst3 : public Steinberg::Vst::SingleComponentEffect,
                   public Steinberg::Vst::IMidiMapping,
                   public Steinberg::Vst::INoteExpressionController,
                   public Clap::IHost,
                   public Clap::IAutomation,
                   public os::IPlugObject
{
public:
  using super = Steinberg::Vst::SingleComponentEffect;

  static FUnknown *createInstance(void *context);

  ClapAsVst3(Clap::Library *lib, int number, void *context)
      : super(), Steinberg::Vst::IMidiMapping(), Steinberg::Vst::INoteExpressionController(), _library(lib),
        _libraryIndex(number), _creationcontext(context)
  {
  }

  //---from IComponent-----------------------
  tresult PLUGIN_API initialize(FUnknown *context) override;
  tresult PLUGIN_API terminate() override;
  tresult PLUGIN_API setActive(TBool state) override;
  tresult PLUGIN_API process(Vst::ProcessData &data) override;
  tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override;
  tresult PLUGIN_API setState(IBStream *state) override;
  tresult PLUGIN_API getState(IBStream *state) override;
  uint32 PLUGIN_API getLatencySamples() override;
  uint32 PLUGIN_API getTailSamples() override;
  tresult PLUGIN_API setupProcessing(Vst::ProcessSetup &newSetup) override;
  tresult PLUGIN_API setProcessing(TBool state) override;
  tresult PLUGIN_API setBusArrangements(Vst::SpeakerArrangement *inputs, int32 numIns, Vst::SpeakerArrangement *outputs,
                                        int32 numOuts) override;
  tresult PLUGIN_API getBusArrangement(Vst::BusDirection dir, int32 index, Vst::SpeakerArrangement &arr) override;
  tresult PLUGIN_API activateBus(Vst::MediaType type, Vst::BusDirection dir, int32 index, TBool state) override;

  //----from IEditControllerEx1--------------------------------
  IPlugView *PLUGIN_API createView(FIDString name) override;
  /** Gets for a given paramID and normalized value its associated string representation. */
  tresult PLUGIN_API getParamStringByValue(Vst::ParamID id, Vst::ParamValue valueNormalized /*in*/,
                                           Vst::String128 string /*out*/) override;
  /** Gets for a given paramID and string its normalized value. */
  tresult PLUGIN_API getParamValueByString(Vst::ParamID id, Vst::TChar *string /*in*/,
                                           Vst::ParamValue &valueNormalized /*out*/) override;

  //----from IMidiMapping--------------------------------------
  tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel, Vst::CtrlNumber midiControllerNumber,
                                                 Vst::ParamID &id /*out*/) override;

#if 1
  //----from INoteExpressionController-------------------------
  /** Returns number of supported note change types for event bus index and channel. */
  int32 PLUGIN_API getNoteExpressionCount(int32 busIndex, int16 channel) override;

  /** Returns note change type info. */
  tresult PLUGIN_API getNoteExpressionInfo(int32 busIndex, int16 channel, int32 noteExpressionIndex,
                                           Vst::NoteExpressionTypeInfo &info /*out*/) override;

  /** Gets a user readable representation of the normalized note change value. */
  tresult PLUGIN_API getNoteExpressionStringByValue(int32 busIndex, int16 channel, Vst::NoteExpressionTypeID id,
                                                    Vst::NoteExpressionValue valueNormalized /*in*/,
                                                    Vst::String128 string /*out*/) override;

  /** Converts the user readable representation to the normalized note change value. */
  tresult PLUGIN_API getNoteExpressionValueByString(int32 busIndex, int16 channel, Vst::NoteExpressionTypeID id,
                                                    const Vst::TChar *string /*in*/,
                                                    Vst::NoteExpressionValue &valueNormalized /*out*/) override;
#endif
  //---Interface--------------------------------------------------------------------------
  OBJ_METHODS(ClapAsVst3, SingleComponentEffect)
  DEFINE_INTERFACES
  // since the macro above opens a local function, this code is being executed during QueryInterface() :)
  if (::Steinberg::FUnknownPrivate::iidEqual(iid, IMidiMapping::iid))
  {
    // when queried for the IMididMapping interface, check if the CLAP supports MIDI dialect on the MIDI Input busses
    // and only return IMidiMapping then
    if (_useIMidiMapping)
    {
      DEF_INTERFACE(IMidiMapping)
    }
  }
  DEF_INTERFACE(INoteExpressionController)
  // tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override;
  END_DEFINE_INTERFACES(SingleComponentEffect)
  REFCOUNT_METHODS(SingleComponentEffect);

  //---Clap::IHost------------------------------------------------------------------------

  void setupWrapperSpecifics(const clap_plugin_t *plugin) override;

  void setupAudioBusses(const clap_plugin_t *plugin, const clap_plugin_audio_ports_t *audioports) override;
  void setupMIDIBusses(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *noteports) override;
  void setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params) override;

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
  bool register_timer(uint32_t period_ms, clap_id *timer_id) override;
  bool unregister_timer(clap_id timer_id) override;

#if LIN
  bool register_fd(int fd, clap_posix_fd_flags_t flags) override;
  bool modify_fd(int fd, clap_posix_fd_flags_t flags) override;
  bool unregister_fd(int fd) override;
#endif

public:
  //----from IPlugObject
  void onIdle() override;

private:
  // from Clap::IAutomation
  void onBeginEdit(clap_id id) override;
  void onPerformEdit(const clap_event_param_value_t *value) override;
  void onEndEdit(clap_id id) override;

  // information function to enable/disable the IMIDIMapping interface
  bool checkMIDIDialectSupport();

private:
  // helper functions
  void addAudioBusFrom(const clap_audio_port_info_t *info, bool is_input);
  void addMIDIBusFrom(const clap_note_port_info_t *info, uint32_t index, bool is_input);
  void updateAudioBusses();

  Vst::UnitID getOrCreateUnitInfo(const char *modulename);
  std::map<std::string, Vst::UnitID> _moduleToUnit;

  Clap::Library *_library = nullptr;
  int _libraryIndex = 0;
  std::shared_ptr<Clap::Plugin> _plugin;
  ClapHostExtensions *_hostextensions = nullptr;
  clap_plugin_as_vst3_t *_vst3specifics = nullptr;
  Clap::ProcessAdapter *_processAdapter = nullptr;
  WrappedView *_wrappedview = nullptr;

  void *_creationcontext; // context from the CLAP library

  // plugin state
  bool _active = false;
  bool _processing = false;
  std::mutex _processingLock;
  std::atomic_bool _requestedFlush = false;
  std::atomic_bool _requestUICallback = false;

  // the queue from audiothread to UI thread
  util::fixedqueue<queueEvent, 8192> _queueToUI;

  // for IMidiMapping
  bool _useIMidiMapping = false;
  Vst::ParamID _IMidiMappingIDs[16][Vst::ControllerNumbers::kCountCtrlNumber] = {}; // 16 MappingIDs for 16 Channels
  bool _IMidiMappingEasy = true;
  uint8_t _numMidiChannels = 16;
  uint32_t _largestBlocksize = 0;

  // for timer
  struct TimerObject
  {
    uint32_t period = 0; // if period is 0 the entry is unused (and can be reused)
    uint64_t nexttick = 0;
    clap_id timer_id = 0;
#if LIN
    Steinberg::IPtr<Steinberg::Linux::ITimerHandler> handler = nullptr;
#endif
  };
  std::vector<TimerObject> _timersObjects;

#if LIN
  Steinberg::IPtr<Steinberg::Linux::ITimerHandler> _idleHandler;

  void attachTimers(Steinberg::Linux::IRunLoop *);
  void detachTimers(Steinberg::Linux::IRunLoop *);
  Steinberg::Linux::IRunLoop *_iRunLoop{nullptr};
#endif

#if LIN
  // for timer
  struct PosixFDObject
  {
    PosixFDObject(int f, clap_posix_fd_flags_t fl) : fd(f), flags(fl) {}
    int fd = 0;
    clap_posix_fd_flags_t flags = 0;
    Steinberg::IPtr<Steinberg::Linux::IEventHandler> handler = nullptr;
  };
  std::vector<PosixFDObject> _posixFDObjects;

  void attachPosixFD(Steinberg::Linux::IRunLoop *);
  void detachPosixFD(Steinberg::Linux::IRunLoop *);
#endif

public:
  void fireTimer(clap_id timer_id);
  void firePosixFDIsSet(int fd, clap_posix_fd_flags_t flags);

private:
  // INoteExpression
  Vst::NoteExpressionTypeContainer _noteExpressions;

  // add a copiler options for which NE to support
  uint32_t _expressionmap =
#if CLAP_SUPPORTS_ALL_NOTE_EXPRESSIONS
      clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_ALL;
#else
      clap_supported_note_expressions::AS_VST3_NOTE_EXPRESSION_PRESSURE;
#endif
};
