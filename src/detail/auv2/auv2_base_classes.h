#pragma once

/*
 Copyright (c) 2023 Timo Kaluza (defiantnerd)

 This file i spart of the clap-wrappers project which is released under MIT License

 See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.
 
 WrapAsAUV2 is the wrapper class for any AUv2 version of a clap.

 It is very unlikely you would need to edit this class since it almost entirely handles everything.
 You just go ahead and write your CLAP.
 
 */

#include <AudioUnitSDK/AUBase.h>
#include "auv2_shared.h"
#include <iostream>
#include <memory>
#include <map>

#include "process.h"
#include "parameter.h"
#include "detail/shared/fixedqueue.h"
#include "detail/os/osutil.h"
#include "detail/clap/automation.h"

enum class AUV2_Type : uint32_t
{
  aufx_effect = 1,
  aumu_musicdevice = 2,
  aumi_noteeffect = 3

};
namespace free_audio::auv2_wrapper
{

class queueEvent
{
 public:
  typedef enum class type
  {
    editstart,
    editvalue,
    editend,
    triggerUICall,
  } type_t;
  type_t _type;
  union
  {
    clap_id _id;
    clap_event_param_value_t _value;
  } _data;
};

class TriggerUICallback : public queueEvent
{
 public:
  TriggerUICallback() : queueEvent()
  {
    this->_type = type::triggerUICall;
  }
};

class BeginEvent : public queueEvent
{
 public:
  BeginEvent(clap_id id) : queueEvent()
  {
    this->_type = type::editstart;
    _data._id = id;
  }
};

class EndEvent : public queueEvent
{
 public:
  EndEvent(clap_id id) : queueEvent()
  {
    this->_type = type::editend;
    _data._id = id;
  }
};

class ValueEvent : public queueEvent
{
 public:
  ValueEvent(const clap_event_param_value_t* value) : queueEvent()
  {
    _type = type::editvalue;
    _data._value = *value;
  }
};

class WrapAsAUV2 : public ausdk::AUBase,
                   public Clap::IHost,
                   public Clap::IAutomation,
                   public os::IPlugObject
{
  using Base = ausdk::AUBase;

 public:
  explicit WrapAsAUV2(AUV2_Type type, const std::string& clapname, const std::string& clapid, int idx,
                      AudioComponentInstance ci);
  virtual ~WrapAsAUV2();

 private:
  AUV2_Type _autype;

  bool initializeClapDesc();

 public:
  // the very very reduced state machine
  OSStatus Initialize() override;
  OSStatus Start() override;
  OSStatus Stop() override;
  void Cleanup() override;

  // latency/tailtime/processing
  virtual Float64 GetLatency() override;
  virtual Float64 GetTailTime() override;
  virtual bool SupportsTail() override
  {
    return false;
  }

  bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override
  {
    return true;
  }

  bool CanScheduleParameters() const override
  {
    return true;
  }

  // AU Properties
  OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                           UInt32& outDataSize, bool& outWritable) override;
  OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                       void* outData) override;
  OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                       const void* inData, UInt32 inDataSize) override;

  // Render Notification
  OSStatus SetRenderNotification(AURenderCallback inProc, void* inRefCon) override;
  OSStatus RemoveRenderNotification(AURenderCallback inProc, void* inRefCon) override;

  OSStatus MIDIEvent(UInt32 inStatus, UInt32 inData1, UInt32 inData2,
                     UInt32 inOffsetSampleFrame) override
  {
    const UInt32 strippedStatus = inStatus & 0xf0U;  // NOLINT
    const UInt32 channel = inStatus & 0x0fU;         // NOLINT

    if (_processAdapter)
    {
      _processAdapter->addMIDIEvent(inStatus, inData1, inData2, inOffsetSampleFrame);
    }
    (void)strippedStatus;
    (void)channel;
    return noErr;  //  HandleMIDIEvent(strippedStatus, channel, inData1, inData2, inOffsetSampleFrame);
  }

  OSStatus SysEx(const UInt8* inData, UInt32 inLength) override
  {
    return noErr;
  }

  // Notes
  OSStatus StartNote(MusicDeviceInstrumentID /*inInstrument*/, MusicDeviceGroupID /*inGroupID*/,
                     NoteInstanceID* /*outNoteInstanceID*/, UInt32 /*inOffsetSampleFrame*/,
                     const MusicDeviceNoteParams& /*inParams*/) override
  {
    // _processAdapter
    // _processAdapter->addMIDIEvent(, <#UInt32 inData1#>, <#UInt32 inData2#>, <#UInt32 inOffsetSampleFrame#>)
    return kAudio_UnimplementedError;
  }

  OSStatus StopNote(MusicDeviceGroupID /*inGroupID*/, NoteInstanceID /*inNoteInstanceID*/,
                    UInt32 /*inOffsetSampleFrame*/) override
  {
    return kAudio_UnimplementedError;
  }

  // unfortunately hidden in the base c++ file
  static void AddNumToDictionary(CFMutableDictionaryRef dict, CFStringRef key, SInt32 value)
  {
    const CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, num);
    CFRelease(num);
  }

  OSStatus SaveState(CFPropertyListRef* ptPList) override;
  OSStatus RestoreState(CFPropertyListRef plist) override;

  // render
  OSStatus Render(AudioUnitRenderActionFlags& inFlags, const AudioTimeStamp& inTimeStamp,
                  UInt32 inFrames) override;

  OSStatus GetParameterList(AudioUnitScope inScope, AudioUnitParameterID* outParameterList,
                            UInt32& outNumParameters) override;
  // outParameterList may be a null pointer
  OSStatus GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID,
                            AudioUnitParameterInfo& outParameterInfo) override;

  OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                        AudioUnitParameterValue inValue, UInt32 /*inBufferOffsetInFrames*/) override;

  // ---------------- Clap::IHost
  void mark_dirty() override
  {
  }
  void restartPlugin() override
  {
  }
  void request_callback() override
  {
    _queueToUI.push(TriggerUICallback());
  }

  void setupWrapperSpecifics(const clap_plugin_t* plugin)
      override;  // called when a wrapper could scan for wrapper specific plugins

  void setupAudioBusses(const clap_plugin_t* plugin,
                        const clap_plugin_audio_ports_t* audioports) override final;
  void setupMIDIBusses(const clap_plugin_t* plugin,
                       const clap_plugin_note_ports_t* noteports)
      override final;  // called from initialize() to allow the setup of MIDI ports
  void setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params) override final;

  void param_rescan(clap_param_rescan_flags flags) override
  {
  }  // ext_host_params
  void param_clear(clap_id param, clap_param_clear_flags flags) override
  {
  }
  void param_request_flush() override
  {
  }

  void latency_changed() override;

  void tail_changed() override;

  bool gui_can_resize() override
  {
    return false;
  }
  bool gui_request_resize(uint32_t width, uint32_t height) override
  {
    return false;
  }
  bool gui_request_show() override
  {
    return false;
  }
  bool gui_request_hide() override
  {
    return false;
  }

  bool register_timer(uint32_t period_ms, clap_id* timer_id) override
  {
    return false;
  }
  bool unregister_timer(clap_id timer_id) override
  {
    return false;
  }

  // --------------- IAutomation
  void onBeginEdit(clap_id id) override;
  void onPerformEdit(const clap_event_param_value_t* value) override;
  void onEndEdit(clap_id id) override;

  // --------------- IPlugObject
  void onIdle() override;

 protected:
  void addAudioBusFrom(int bus, const clap_audio_port_info_t* info, bool is_input);

 private:
  // ---------------- glue code stuff
  void addInputBus(int bus, const clap_audio_port_info_t* info);
  void addOutputBus(int bus, const clap_audio_port_info_t* info);

  void activateCLAP();
  void deactivateCLAP();
  bool IsBypassEffect()
  {
    return false;
  }
  void SetBypassEffect(bool bypass){};

  // --------------- internals

  // the wrapped CLAP:
  std::string _clapname;
  std::string _clapid;
  int _idx;
  os::State _os_attached;

  const clap_plugin_descriptor_t* _desc{nullptr};
  std::shared_ptr<Clap::Plugin> _plugin = nullptr;

  std::unique_ptr<Clap::AUv2::ProcessAdapter> _processAdapter;
  std::atomic<bool> _initialized = false;

  // some info about the wrapped clap
  uint32_t _midi_preferred_dialect = 0;
  bool _midi_wants_midi_input = false;  // takes any input
  bool _midi_understands_midi2 = false;
  std::map<uint32_t, std::unique_ptr<Clap::AUv2::Parameter>> _parametertree;

  CFStringRef _current_program_name = 0;

  // the queue from audiothread to UI thread
  ClapWrapper::detail::shared::fixedqueue<queueEvent, 8192> _queueToUI;
};

}  // namespace free_audio::auv2_wrapper
