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

namespace free_audio::auv2_wrapper
{

class WrapAsAUV2 : public ausdk::AUBase, public Clap::IHost
{
  using Base = ausdk::AUBase;
  
public:
  explicit WrapAsAUV2(const std::string &clapname, const std::string &clapid, int idx,
                      AudioComponentInstance ci)
  : Base{ci, 0,1}, Clap::IHost(), _clapname{clapname}, _clapid{clapid}, _idx{idx}
  {}
private:

  
  bool initializeClapDesc();
  
public:
  // the very very reduced state machine
  OSStatus Initialize() override;
  OSStatus Start() override;
  OSStatus Stop() override;
  void Cleanup() override;
  
  bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override
  {
    return true;
  }

  bool CanScheduleParameters() const override
  {
    return true;
  }
  
  // AU Properties
  OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
    AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable) override;
  OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
    AudioUnitElement inElement, void* outData) override;
  OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
    AudioUnitElement inElement, const void* inData, UInt32 inDataSize) override;
  
  // Render Notification
  OSStatus SetRenderNotification(AURenderCallback inProc, void* inRefCon) override;
  OSStatus RemoveRenderNotification(AURenderCallback inProc, void* inRefCon) override;
  
  OSStatus MIDIEvent(
    UInt32 inStatus, UInt32 inData1, UInt32 inData2, UInt32 inOffsetSampleFrame) override
  {
    const UInt32 strippedStatus = inStatus & 0xf0U; // NOLINT
    const UInt32 channel = inStatus & 0x0fU;        // NOLINT

    (void) strippedStatus;
    (void) channel;
    return noErr; //  HandleMIDIEvent(strippedStatus, channel, inData1, inData2, inOffsetSampleFrame);
  }
  
  OSStatus SysEx(const UInt8* inData, UInt32 inLength) override { return noErr; }

  
  // Notes
  OSStatus StartNote(MusicDeviceInstrumentID /*inInstrument*/,
    MusicDeviceGroupID /*inGroupID*/, NoteInstanceID* /*outNoteInstanceID*/,
    UInt32 /*inOffsetSampleFrame*/, const MusicDeviceNoteParams& /*inParams*/) override
  {
    return kAudio_UnimplementedError;
  }

  OSStatus StopNote(MusicDeviceGroupID /*inGroupID*/, NoteInstanceID /*inNoteInstanceID*/,
                    UInt32 /*inOffsetSampleFrame*/) override
  {
    return kAudio_UnimplementedError;
  }
  
  // ---------------- Clap::IHost
  void mark_dirty() override {}
  void restartPlugin() override {}
  void request_callback() override {}

  void setupWrapperSpecifics(
                             const clap_plugin_t* plugin) override; // called when a wrapper could scan for wrapper specific plugins

  void setupAudioBusses(
      const clap_plugin_t* plugin,
      const clap_plugin_audio_ports_t*
                        audioports) override;
  void setupMIDIBusses(
      const clap_plugin_t* plugin,
      const clap_plugin_note_ports_t*
                      noteports ) override;  // called from initialize() to allow the setup of MIDI ports
  void setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params) override {}

  void param_rescan(clap_param_rescan_flags flags) override {}  // ext_host_params
  void param_clear(clap_id param, clap_param_clear_flags flags) override {}
  void param_request_flush() override {}

  void latency_changed() override {}

  void tail_changed() override {}
  
  bool gui_can_resize() override { return false; }
  bool gui_request_resize(uint32_t width, uint32_t height) override { return false;}
  bool gui_request_show() override {return false;}
  bool gui_request_hide() override {return false;}

  bool register_timer(uint32_t period_ms, clap_id* timer_id) override {return false;}
  bool unregister_timer(clap_id timer_id) override {return false;}

protected:
  void addAudioBusFrom(int bus, const clap_audio_port_info_t* info, bool is_input);
private:
  
  // ---------------- glue code stuff
  void addInputBus(int bus, const clap_audio_port_info_t* info);
  void addOutputBus(int bus, const clap_audio_port_info_t* info);
  
  bool IsBypassEffect() { return false; }
  void SetBypassEffect(bool bypass) {};
  
  // --------------- internals
  
  // the wrapped CLAP:
  std::string _clapname;
  std::string _clapid;
  int _idx;
  const clap_plugin_descriptor_t *_desc{nullptr};
  std::shared_ptr<Clap::Plugin> _plugin = nullptr;
  
  // Clap::AU::ProcessAdapter* _processAdapter = nullptr;
  
  
};

}  // namespace free_audio::auv2_wrapper
