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
#include <CoreMIDI/CoreMIDI.h>
#include "auv2_shared.h"
#include <iostream>
#include <memory>
#include <map>

#include "process.h"
#include "parameter.h"
#include "detail/shared/fixedqueue.h"
#include "detail/os/osutil.h"
#include "detail/clap/automation.h"

#define NDUAL_SCHEDULING_ENABLED 1

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

class Clumps
{
 public:
  void reset();
  UInt32 addClump(const char* fullpath);
  const char* getClump(UInt32 id);

 private:
  UInt32 _lastclump = 0;
  std::map<std::string, UInt32> _clumps;
};

void Clumps::reset()
{
  _clumps.clear();
  _lastclump = 0;
}

const char* Clumps::getClump(UInt32 id)
{
  for (const auto& c : _clumps)
  {
    if (c.second == id)
    {
      return c.first.c_str();
    }
  }
  return nullptr;
}

UInt32 Clumps::addClump(const char* fullpath)
{
  auto r = _clumps.find(fullpath);
  if (r == _clumps.end())
  {
    _clumps[fullpath] = ++_lastclump;
    return _lastclump;
  }

  return r->second;
}

class MIDIOutput
{
 public:
  MIDIOutput() = delete;
  ~MIDIOutput()
  {
  }
  MIDIOutput(int auport, const clap_note_port_info& info);
  MIDIOutput(const MIDIOutput&) = delete;
  MIDIOutput(MIDIOutput&&) = delete;
  MIDIPacketList* getMIDIPacketList()
  {
    return _midiPacketList;
  }
  bool addNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
  bool addNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
  bool addMIDI3Byte(const uint8_t* threebytes);

  void clear();
  const clap_note_port_info _info;
  const int _auport;
  bool hasEvents() const
  {
    return _numEvents != 0;
  }

 private:
  MIDIPacket* _current = nullptr;
  MIDIPacketList* _midiPacketList = nullptr;
  uint8_t _buffer[2048];
  uint32_t _numEvents = 0;
};

MIDIOutput::MIDIOutput(int auport, const clap_note_port_info& info) : _info(info), _auport(auport)
{
  _midiPacketList = (MIDIPacketList*)_buffer;
  _current = MIDIPacketListInit(_midiPacketList);
  _numEvents = 0;
}

void MIDIOutput::clear()
{
  _current = MIDIPacketListInit(_midiPacketList);
  _numEvents = 0;
}

bool MIDIOutput::addNoteOn(uint8_t channel, uint8_t note, uint8_t velocity)
{
  uint8_t ev[3] = {static_cast<uint8_t>((uint8_t)0x90u | (channel & 0xF)),
                   static_cast<uint8_t>((note & 0x7F)), static_cast<uint8_t>((velocity & 0x7F))};
  _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 3, (Byte*)ev);
  ++_numEvents;
  return (_current != nullptr);
}
bool MIDIOutput::addNoteOff(uint8_t channel, uint8_t note, uint8_t velocity)
{
  uint8_t ev[3] = {static_cast<uint8_t>((uint8_t)0x80u | (channel & 0xF)),
                   static_cast<uint8_t>((note & 0x7F)), static_cast<uint8_t>((velocity & 0x7F))};
  _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 3, (Byte*)ev);
  ++_numEvents;
  return (_current != nullptr);
}

bool MIDIOutput::addMIDI3Byte(const uint8_t* threebytes)
{
  auto cmd = (threebytes[0] >> 4) & 0xFu;
  switch (cmd)
  {
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0E:
      _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 3, threebytes);
      break;
    case 0x0C:
    case 0x0D:
      _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 2, threebytes);
      break;
    default:
      return false;
  }

  ++_numEvents;
  return (_current != nullptr);
}

class WrapAsAUV2 : public ausdk::AUBase,
                   public Clap::IHost,
                   public Clap::IAutomation,
                   public os::IPlugObject,
                   public Clap::AUv2::IMIDIOutputs
{
  using Base = ausdk::AUBase;

 public:
  explicit WrapAsAUV2(AUV2_Type type, const std::string& clapname, const std::string& clapid, int idx,
                      AudioComponentInstance ci);
  virtual ~WrapAsAUV2();

 protected:
  void PostConstructor() override;

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

  std::vector<AUChannelInfo> cinfo;

  UInt32 SupportedNumChannels(const AUChannelInfo** outInfo) override;
  bool ValidFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                   const AudioStreamBasicDescription& inNewFormat) override;
  OSStatus ChangeStreamFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                              const AudioStreamBasicDescription& inPrevFormat,
                              const AudioStreamBasicDescription& inNewFormat) override;

 protected:
  UInt32 GetAudioChannelLayout(AudioUnitScope scope, AudioUnitElement element,
                               AudioChannelLayout* outLayoutPtr, bool& outWritable) override;

 public:
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
                     NoteInstanceID* outNoteInstanceID, UInt32 inOffsetSampleFrame,
                     const MusicDeviceNoteParams& inParams) override
  {
    // _processAdapter
    // _processAdapter->addMIDIEvent(, <#UInt32 inData1#>, <#UInt32 inData2#>, <#UInt32 inOffsetSampleFrame#>);
    *outNoteInstanceID = inParams.mPitch;
    return MIDIEvent(0x90, inParams.mPitch, inParams.mVelocity, inOffsetSampleFrame);
  }

  OSStatus StopNote(MusicDeviceGroupID /*inGroupID*/, NoteInstanceID inNoteInstanceID,
                    UInt32 inOffsetSampleFrame) override
  {
    return MIDIEvent(0x80, inNoteInstanceID, 0, inOffsetSampleFrame);
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

  OSStatus CopyClumpName(AudioUnitScope inScope, UInt32 inClumpID, UInt32 inDesiredNameLength,
                         CFStringRef* outClumpName) override;

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

  void param_rescan(clap_param_rescan_flags flags) override;

  // ext_host_params
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

  const char* host_get_name() override
  {
    char text[65];
    // No need to release any of these "Get" functions.
    // https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFMemoryMgmt/Concepts/Ownership.html#//apple_ref/doc/uid/20001148-SW1

    CFBundleRef applicationBundle = CFBundleGetMainBundle();
    if (applicationBundle != NULL)
    {
      CFStringRef myProductString =
          (CFStringRef)CFBundleGetValueForInfoDictionaryKey(applicationBundle, kCFBundleNameKey);

      if (myProductString)
      {
        CFStringGetCString(myProductString, text, 64, kCFStringEncodingUTF8);
        _hostname = text;
      }
      else
      {
        CFStringRef applicationBundleID = CFBundleGetIdentifier(applicationBundle);
        if (applicationBundleID)
        {
          CFStringGetCString(applicationBundleID, text, 64, kCFStringEncodingUTF8);
          _hostname = text;
        }
      }
      CFStringRef myVersionString =
          (CFStringRef)CFBundleGetValueForInfoDictionaryKey(applicationBundle, kCFBundleVersionKey);
      if (myVersionString)
      {
        CFStringGetCString(myVersionString, text, 64, kCFStringEncodingUTF8);
        _hostname.append(" (");
        _hostname.append(text);
        _hostname.append(")");
      }
      _hostname.append(" (CLAP-as-AUv2)");
    }
    return _hostname.c_str();
  }

  // --------------- IAutomation
  void onBeginEdit(clap_id id) override;
  void onPerformEdit(const clap_event_param_value_t* value) override;
  void onEndEdit(clap_id id) override;

  // --------------- IPlugObject
  void onIdle() override;

  // context menu extension
  bool supportsContextMenu() const override
  {
    return false;
  }
  bool context_menu_populate(const clap_context_menu_target_t* target,
                             const clap_context_menu_builder_t* builder) override
  {
    return false;
  }
  bool context_menu_perform(const clap_context_menu_target_t* target, clap_id action_id) override
  {
    return false;
  }
  bool context_menu_can_popup() override
  {
    return false;
  }
  bool context_menu_popup(const clap_context_menu_target_t* target, int32_t screen_index, int32_t x,
                          int32_t y) override
  {
    return false;
  }

  // --------------- IMIDIOutputs
  void send(const Clap::AUv2::clap_multi_event_t& event) override;

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
  // std::vector<clap_note_port_info_t> _midi_outports_info;

  std::string _hostname = "CLAP-as-AUv2";

#ifdef DUAL_SCHEDULING_ENABLED
  bool _midi_dualscheduling_mode = false;
#endif
  std::map<uint32_t, std::unique_ptr<Clap::AUv2::Parameter>> _parametertree;
  Clumps _clumps;

  CFStringRef _current_program_name = 0;

  // ------------- for the MIDI output
  AUMIDIOutputCallbackStruct _midioutput_hostcallback = {nullptr, nullptr};

  // the queue from audiothread to UI thread
  ClapWrapper::detail::shared::fixedqueue<queueEvent, 8192> _queueToUI;

  std::vector<std::unique_ptr<MIDIOutput>> _midi_outports;
};

}  // namespace free_audio::auv2_wrapper
