#pragma once
/*
    CLAP as AAX

    Copyright (c) 2024 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.
    
    This AAX opens a CLAP plugin and matches all corresponding AAX calls to it.
    For the AAX Host it is a AAX plugin, for the CLAP plugin it is a CLAP host.

*/

// clang-format off

#ifdef WIN32
#pragma warning(disable: 5033)  // \aax-sdk-2-8-1\Interfaces\AAX_Atomic.h(191,26): warning C5033: 'register' is no longer a supported storage class
#endif

// clang-format on

// AAX headers
//
// Parent class
#include "AAX_CEffectParameters.h"
// #include "Topology/AAX_CMonolithicParameters.h" <- no!

//
// Describe
#include "AAX_IEffectDescriptor.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IPropertyMap.h"
//
// Utilities
#include "AAX_CAtomicQueue.h"
#include "AAX_IParameter.h"
#include "AAX_IString.h"

// Wrapper
#include "detail/os/osutil.h"
#include "detail/clap/automation.h"
#include "clap_proxy.h"
#include "parameter.h"
#include "process.h"

// cpp stuff
#include <map>
#include <string>
#include <mutex>
#include <vector>

#include "../clap/automation.h"

#include "util.h"

class AAX_ICollection;
class Wrapped_AAX_GUI;
class ClapAsAAX;

constexpr uint32_t gAAXMinBlockSizeInSamples = (1 << AAX_eAudioBufferLengthNative_Min);
constexpr uint32_t gAAXMaxBlockSizeInSamples = (1 << AAX_eAudioBufferLengthNative_Max);

class AAXProcessAdapter
{
 public:
  typedef union clap_multi_event
  {
    clap_event_header_t header;
    clap_event_note_t note;
    clap_event_midi_t midi;
    clap_event_midi_sysex_t sysex;
    clap_event_param_value_t param;
    clap_event_note_expression_t noteexpression;
  } clap_multi_event_t;

  clap_process_t _process;  // process_t for clap

  ~AAXProcessAdapter();
  void applyBusSetting(const clap_plugin_t *plugin, const char *buslayout,
                       const clap_plugin_configurable_audio_ports_t *ext);
  void setupProcessing(const clap_plugin_t *plugin, double samplerate,
                       const clap_plugin_params_t *ext_param, const clap_plugin_audio_ports *ext_audio,
                       Clap::IAutomation *automation, std::vector<clap_id> &gesturedParameters,
                       ParamChangeQueue &inqueue, uint32_t midiportid, bool preferMIDI);
  void process(SAAX_Wrapper_AlgorithmicContext *context);
  void flush();

 private:
  // the plugin
  const clap_plugin_t *_plugin = nullptr;
  const clap_plugin_params_t *_ext_param = nullptr;
  // for automation gestures
  std::vector<clap_id> *_gesturedparameters = nullptr;

  Clap::IAutomation *_automation = nullptr;

  // for Note Expressions - check if we need that
  struct ActiveNote
  {
    bool used = false;
    int32_t note_id;  // -1 if unspecified, otherwise >=0
    int16_t port_index;
    int16_t channel;  // 0..15
    int16_t key;      // 0..127
  };
  std::vector<ActiveNote> _activeNotes;

  // raw pointer for compatibility with C API
  clap_audio_buffer_t *_input_ports = nullptr;
  clap_audio_buffer_t *_output_ports = nullptr;
  clap_event_transport_t _transport = {};
  clap_input_events_t _in_events = {};
  clap_output_events_t _out_events = {};
  clap_process_t _proc;

  std::vector<clap_multi_event_t> _events;
  std::vector<size_t> _eventindices;
  double _samplerate = 44100;
  float *_silent_input = nullptr;
  float *_silent_output = nullptr;

  ParamChangeQueue *_inqueue;

  void sortEventIndices();

  static bool output_events_try_push(const struct clap_output_events *list,
                                     const clap_event_header_t *event);

  bool enqueueOutputEvent(const clap_event_header_t *event);
  void addToActiveNotes(const clap_event_note *note);
  void removeFromActiveNotes(const clap_event_note *note);

  // the functions for the event list callback
  static uint32_t CLAP_ABI input_events_size(const struct clap_input_events *list);
  static const clap_event_header_t *CLAP_ABI input_events_get(const struct clap_input_events *list,
                                                              uint32_t index);

  // MIDI
  uint32_t _midi_first_portid = 0;
  bool _midi_prefer_mididialect = true;
};

AAX_Result GetEffectDescriptions(AAX_ICollection *outDescriptions);
AAX_CEffectParameters *AAX_CALLBACK ClapAsAAX_Create();
AAX_CEffectParameters *AAX_CALLBACK ClapAsAAX_Create_WithConfig(const char *effect_id, int busconfig);

class ClapAsAAX : public AAX_CEffectParameters,
                  public Clap::IHost,
                  public Clap::IAutomation,
                  public os::IPlugObject
{
 public:
  friend class Wrapped_AAX_GUI;
  ClapAsAAX();
  ClapAsAAX(const char *effectid, int busconfig);
  virtual ~ClapAsAAX();
  AAX_Result EffectInit() override;
  AAX_Result ResetFieldData(AAX_CFieldIndex iFieldIndex, void *oData, uint32_t iDataSize) const override;
  AAX_Result TimerWakeup() override;
  AAX_Result GetParameterIsAutomatable(AAX_CParamID iParameterID, AAX_CBoolean *itIs) const override;
  AAX_Result GetParameterNumberOfSteps(AAX_CParamID iParameterID, int32_t *aNumSteps) const override;
  AAX_Result GetParameterValueString(AAX_CParamID iParameterID, AAX_IString *oValueString,
                                     int32_t iMaxLength) const override;
  AAX_Result GetParameterValueFromString(AAX_CParamID iParameterID, double *oValuePtr,
                                         const AAX_IString &iValueString) const override;
  AAX_Result GetParameterStringFromValue(AAX_CParamID iParameterID, double value,
                                         AAX_IString *valueString, int32_t maxLength) const override;
  AAX_Result GetParameterName(AAX_CParamID iParameterID, AAX_IString *oName) const override;
  AAX_Result GetParameterNameOfLength(AAX_CParamID iParameterID, AAX_IString *oName,
                                      int32_t iNameLength) const override;

  // override to catch value changes and pass it to the local queue
  AAX_Result UpdateParameterNormalizedValue(AAX_CParamID iParameterID, double iValue,
                                            AAX_EUpdateSource iSource) override;

  //---The Clunky Chunk-------------------------------------------------------------------
  AAX_Result GetNumberOfChunks(int32_t *oNumChunks) const override;
  AAX_Result GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID *oChunkID) const override;
  AAX_Result GetChunkSize(AAX_CTypeID iChunkID, uint32_t *oSize) const override;
  AAX_Result GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk *oChunk) const override;
  AAX_Result SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk *iChunk) override;

  AAX_Result NotificationReceived(/* AAX_ENotificationEvent */ AAX_CTypeID inNotificationType,
                                  const void *inNotificationData,
                                  uint32_t inNotificationDataSize) override;

  //---Clap::IHost------------------------------------------------------------------------

  void setupWrapperSpecifics(const clap_plugin_t *plugin) override;

  void setupAudioBusses(const clap_plugin_t *plugin,
                        const clap_plugin_audio_ports_t *audioports) override;
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

  bool track_info_get(clap_track_info_t *info) override;
  const char *host_get_name() override;

  bool supportsContextMenu() const override;
  // context_menu
  bool context_menu_populate(const clap_context_menu_target_t *target,
                             const clap_context_menu_builder_t *builder) override;
  bool context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id) override;
  bool context_menu_can_popup() override;
  bool context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index, int32_t x,
                          int32_t y) override;

  void onIdle() override;

  void activatePlugin();
  void deactivatePlugin();
  void startProcessing();
  void stopProcessing();

  void process(SAAX_Wrapper_AlgorithmicContext *context);

 protected:
  Clap::Library *_library = nullptr;
  std::shared_ptr<Clap::Plugin> _plugin;

  void *_creationcontext = nullptr;  // context from the CLAP library
  os::State _os_attached;

  std::string _wrapper_hostname = "CLAP-As-AAX-Wrapper";

  // _parameterMap maps the AAX CParamID to the wrapped parameter
  std::map<std::string, std::shared_ptr<AAXWrappedParameterInfo_t>> _parameterMap;
  std::map<uint32_t, std::shared_ptr<AAXWrappedParameterInfo_t>> _parameterMapCLAP;

  Wrapped_AAX_GUI *_aax_view = nullptr;

  AAX_IController *_aax_ctrl = nullptr;

  mutable Clap::StateMemento _state;

  std::atomic<bool> _activated{false};
  std::atomic<bool> _processing{false};
  std::atomic<bool> _wants_on_main_thread = false;
  std::atomic<bool> _flushRequested = false;
  uint32_t _latency = 0;

  std::unique_ptr<AAXProcessAdapter> _processAdapter;

  struct TimerObject
  {
    uint32_t period_ms = 0;  // 0 means unused / available for reuse
    uint64_t nexttick = 0;
    clap_id timer_id = 0;
  };
  std::vector<TimerObject> _timerObjects;

  std::vector<clap_audio_port_configuration_request> _configuration_requests;

  uint32_t _midi_first_portid = 0;
  bool _midi_prefer_mididialect = true;

  ParamChangeQueue _paramsToProcess;

 private:
  std::string _predetermined_effectid;
  int _predetermined_busconfig = 0;

  // from Clap::IAutomation
  std::vector<clap_id> _gesturedparameters;

  void onBeginEdit(clap_id id) override;
  void onPerformEdit(const clap_event_param_value_t *value) override;
  void onEndEdit(clap_id id) override;
};
