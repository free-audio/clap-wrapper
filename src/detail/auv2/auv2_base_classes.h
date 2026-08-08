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
#include <mutex>
#include <optional>
#include <string>

#include "process.h"
#include "parameter.h"
#include "detail/shared/fixedqueue.h"
#include "detail/shared/midi_translation.h"
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
  } type_t;
  type_t _type;
  union
  {
    clap_id _id;
    clap_event_param_value_t _value;
  } _data;
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
  ValueEvent(const clap_event_param_value_t *value) : queueEvent()
  {
    _type = type::editvalue;
    _data._value = *value;
  }
};

/*
 * The parameter group names, by the clump id the host sees in
 * AudioUnitParameterInfo::clumpID.
 *
 * Guarded, and answering by value, because the two sides of this are on
 * different threads. setupParameters() fills it on the main thread; the host
 * reads it back through kAudioUnitProperty_ParameterClumpName while building its
 * parameter tree, which it is free to do wherever it likes -- Logic uses a
 * parameterTreeBuilderQueue of its own, asynchronously with respect to the
 * property change that asked for the rebuild. Handing out a const char* into the
 * map was therefore a use-after-free, and it was reachable by changing presets
 * with audio running: the next rescan cleared the map while the host was reading
 * from it.
 *
 * There is deliberately no way to empty it. Clump ids outlive the rescan that
 * minted them -- the host keeps the one it read from AudioUnitParameterInfo and
 * asks for its name whenever it likes -- so renumbering is what made them wrong,
 * and clearing is what made them dangerous. addClump() dedupes, so the map
 * converges on the module paths the plugin reports and stops growing.
 */
class Clumps
{
 public:
  UInt32 addClump(const char *fullpath);
  std::optional<std::string> getClump(UInt32 id) const;

 private:
  mutable std::mutex _mutex;
  UInt32 _lastclump = 0;
  std::map<std::string, UInt32> _clumps;
};

inline std::optional<std::string> Clumps::getClump(UInt32 id) const
{
  std::lock_guard<std::mutex> guard(_mutex);
  for (const auto &c : _clumps)
  {
    if (c.second == id)
    {
      return c.first;
    }
  }
  return std::nullopt;
}

inline UInt32 Clumps::addClump(const char *fullpath)
{
  std::lock_guard<std::mutex> guard(_mutex);
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
  MIDIOutput(int auport, const clap_note_port_info &info);
  MIDIOutput(const MIDIOutput &) = delete;
  MIDIOutput(MIDIOutput &&) = delete;
  MIDIPacketList *getMIDIPacketList()
  {
    return _midiPacketList;
  }
#if AUSDK_MIDI2_AVAILABLE
  // parallel Universal MIDI Packet view of the same events (protocol 1.0 / MT 0x2);
  // the framework converts to the host's negotiated protocol on delivery.
  MIDIEventList *getMIDIEventList()
  {
    return _umpList;
  }
#endif
  bool addNoteOn(uint8_t channel, uint8_t note, uint8_t velocity);
  bool addNoteOff(uint8_t channel, uint8_t note, uint8_t velocity);
  bool addMIDI3Byte(const uint8_t *threebytes);
  bool addSysEx(const uint8_t *data, uint32_t size);

  void clear();
  const clap_note_port_info _info;
  const int _auport;
  bool hasEvents() const
  {
    return _numEvents != 0;
  }

 private:
#if AUSDK_MIDI2_AVAILABLE
  // wrap a MIDI 1.0 channel-voice message into one MT 0x2 UMP word
  void appendUMP1(uint8_t status, uint8_t data1, uint8_t data2)
  {
    // _umpCurrent is null when running on macOS < 11 (the list is never
    // initialized there) and once the list is full (MIDIEventListAdd returns
    // nullptr and must not be called with a null curPacket) - drop the event.
    if (!_umpCurrent) return;
    if (__builtin_available(macOS 11.0, *))
    {
      uint32_t word = ClapWrapper::detail::shared::midi1ToUmpWord(status, data1, data2);
      _umpCurrent = MIDIEventListAdd(_umpList, sizeof(_umpBuffer), _umpCurrent, 0, 1, &word);
    }
  }
  // pack a SysEx payload (0xF0/0xF7 framing stripped) into MT 0x3 SysEx7 packets
  void appendUMPSysEx(const uint8_t *data, uint32_t size);
#endif

  MIDIPacket *_current = nullptr;
  MIDIPacketList *_midiPacketList = nullptr;
  uint8_t _buffer[2048];
  uint32_t _numEvents = 0;
#if AUSDK_MIDI2_AVAILABLE
  MIDIEventPacket *_umpCurrent = nullptr;
  MIDIEventList *_umpList = nullptr;
  uint8_t _umpBuffer[2048];
#endif
};

MIDIOutput::MIDIOutput(int auport, const clap_note_port_info &info) : _info(info), _auport(auport)
{
  _midiPacketList = (MIDIPacketList *)_buffer;
  _current = MIDIPacketListInit(_midiPacketList);
  _numEvents = 0;
#if AUSDK_MIDI2_AVAILABLE
  // on macOS < 11 the CoreMIDI EventList API is unavailable; _umpList and
  // _umpCurrent stay null and every UMP append becomes a no-op
  if (__builtin_available(macOS 11.0, *))
  {
    _umpList = (MIDIEventList *)_umpBuffer;
    _umpCurrent = MIDIEventListInit(_umpList, kMIDIProtocol_1_0);
  }
#endif
}

void MIDIOutput::clear()
{
  _current = MIDIPacketListInit(_midiPacketList);
  _numEvents = 0;
#if AUSDK_MIDI2_AVAILABLE
  if (__builtin_available(macOS 11.0, *))
  {
    if (_umpList) _umpCurrent = MIDIEventListInit(_umpList, kMIDIProtocol_1_0);
  }
#endif
}

bool MIDIOutput::addNoteOn(uint8_t channel, uint8_t note, uint8_t velocity)
{
  // once the list is full MIDIPacketListAdd returns nullptr; it must never be
  // called with a null curPacket, so further events this block are dropped
  if (!_current) return false;
  uint8_t ev[3] = {static_cast<uint8_t>((uint8_t)0x90u | (channel & 0xF)),
                   static_cast<uint8_t>((note & 0x7F)), static_cast<uint8_t>((velocity & 0x7F))};
  _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 3, (Byte *)ev);
#if AUSDK_MIDI2_AVAILABLE
  appendUMP1(ev[0], ev[1], ev[2]);
#endif
  ++_numEvents;
  return (_current != nullptr);
}
bool MIDIOutput::addNoteOff(uint8_t channel, uint8_t note, uint8_t velocity)
{
  if (!_current) return false;
  uint8_t ev[3] = {static_cast<uint8_t>((uint8_t)0x80u | (channel & 0xF)),
                   static_cast<uint8_t>((note & 0x7F)), static_cast<uint8_t>((velocity & 0x7F))};
  _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 3, (Byte *)ev);
#if AUSDK_MIDI2_AVAILABLE
  appendUMP1(ev[0], ev[1], ev[2]);
#endif
  ++_numEvents;
  return (_current != nullptr);
}

bool MIDIOutput::addMIDI3Byte(const uint8_t *threebytes)
{
  if (!_current) return false;
  auto cmd = (threebytes[0] >> 4) & 0xFu;
  switch (cmd)
  {
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0E:
      _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 3, threebytes);
#if AUSDK_MIDI2_AVAILABLE
      appendUMP1(threebytes[0], threebytes[1], threebytes[2]);
#endif
      break;
    case 0x0C:
    case 0x0D:
      _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, 2, threebytes);
#if AUSDK_MIDI2_AVAILABLE
      appendUMP1(threebytes[0], threebytes[1], 0);
#endif
      break;
    default:
      return false;
  }

  ++_numEvents;
  return (_current != nullptr);
}

bool MIDIOutput::addSysEx(const uint8_t *data, uint32_t size)
{
  if (!_current || !data || size == 0) return false;
  // MIDIPacketListAdd splits the payload across packets as needed, but the
  // whole list is still bounded by our fixed _buffer; very long SysEx that does
  // not fit is dropped (returns nullptr). The CLAP buffer already contains the
  // full message including the 0xF0/0xF7 framing.
  _current = MIDIPacketListAdd(_midiPacketList, sizeof(_buffer), _current, 0, size, (Byte *)data);
  if (_current == nullptr) return false;
#if AUSDK_MIDI2_AVAILABLE
  appendUMPSysEx(data, size);
#endif
  ++_numEvents;
  return true;
}

#if AUSDK_MIDI2_AVAILABLE
void MIDIOutput::appendUMPSysEx(const uint8_t *data, uint32_t size)
{
  if (__builtin_available(macOS 11.0, *))
  {
    ClapWrapper::detail::shared::packSysEx7(
        data, size,
        [this](uint32_t w0, uint32_t w1)
        {
          // stop appending once the list is full or was never initialized
          // (macOS < 11): MIDIEventListAdd must not be called with a null curPacket
          if (!_umpCurrent) return;
          uint32_t words[2] = {w0, w1};
          _umpCurrent = MIDIEventListAdd(_umpList, sizeof(_umpBuffer), _umpCurrent, 0, 2, words);
        });
  }
}
#endif

class WrapAsAUV2 : public ausdk::AUBase,
                   public Clap::IHost,
                   public Clap::IAutomation,
                   public Clap::AUv2::IMIDIOutputs,
                   public os::IPlugObject
{
  using Base = ausdk::AUBase;

 public:
  explicit WrapAsAUV2(AUV2_Type type, const std::string &clapname, const std::string &clapid, int idx,
                      AudioComponentInstance ci);
  virtual ~WrapAsAUV2();

 protected:
  void PostConstructor() override;

 private:
  AUV2_Type _autype;

  // connection from plugin to view
  ui_connection _uiconn;
  bool _uiIsOpened;

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

  UInt32 SupportedNumChannels(const AUChannelInfo **outInfo) override;
  bool ValidFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                   const AudioStreamBasicDescription &inNewFormat) override;

  // channel count presented by the placeholder busses of an effect facade (a
  // plugin that declares no audio ports at all - see PostConstructor).
  static constexpr UInt32 kPlaceholderFacadeChannels = 2;
  // true when both audio scopes are placeholder-only, i.e. the wrapped CLAP
  // declares no audio input and no audio output ports and we present a silent
  // stereo in/out facade so the unit is a valid aufx.
  bool isEffectFacade();
  OSStatus ChangeStreamFormat(AudioUnitScope inScope, AudioUnitElement inElement,
                              const AudioStreamBasicDescription &inPrevFormat,
                              const AudioStreamBasicDescription &inNewFormat) override;

 protected:
  UInt32 GetAudioChannelLayout(AudioUnitScope scope, AudioUnitElement element,
                               AudioChannelLayout *outLayoutPtr, bool &outWritable) override;

 public:
  bool CanScheduleParameters() const override
  {
    return true;
  }

  // AU Properties
  OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                           UInt32 &outDataSize, bool &outWritable) override;
  OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                       void *outData) override;
  OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                       const void *inData, UInt32 inDataSize) override;

  // Render Notification
  OSStatus SetRenderNotification(AURenderCallback inProc, void *inRefCon) override;
  OSStatus RemoveRenderNotification(AURenderCallback inProc, void *inRefCon) override;

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

#if AUSDK_MIDI2_AVAILABLE
  // MIDI 2.0 / Universal MIDI Packet input. The framework delivers events in the
  // protocol we advertise via kAudioUnitProperty_AudioUnitMIDIProtocol; we honour
  // whatever protocol the list actually carries. MIDI 2.0 channel-voice messages
  // are forwarded raw as CLAP_EVENT_MIDI2 (for plugins that prefer MIDI2); MIDI
  // 1.0 messages reuse the byte-based translation so they pick up the note /
  // note-expression handling of the MIDI1 path.
  OSStatus MIDIEventList(UInt32 inOffsetSampleFrame, const struct MIDIEventList *evtlist) override
  {
    if (!_processAdapter || !evtlist) return noErr;

    const bool midi2 = (evtlist->protocol == kMIDIProtocol_2_0);
    const MIDIEventPacket *pkt = &evtlist->packet[0];
    for (UInt32 p = 0; p < evtlist->numPackets; ++p)
    {
      // MusicDevice.h: each event's sample offset is inOffsetSampleFrame plus
      // the packet's timeStamp (itself a sample offset within the render cycle)
      const UInt32 offset = inOffsetSampleFrame + static_cast<UInt32>(pkt->timeStamp);
      for (UInt32 i = 0; i < pkt->wordCount;)
      {
        const uint32_t w0 = pkt->words[i];
        const uint32_t nWords = ClapWrapper::detail::shared::umpMessageWordCount(w0);
        if (i + nWords > pkt->wordCount) break;  // truncated packet, stop

        const uint32_t mt = (w0 >> 28) & 0xFu;
        if (midi2 && mt == 0x4u)
        {
          if (_midi_understands_midi2)
          {
            // MIDI 2.0 channel voice message, forwarded raw
            _processAdapter->addMIDI2Event(&pkt->words[i], nWords, offset);
          }
          else
          {
            // the plugin's note port never declared the MIDI2 dialect (a host
            // ignoring our advertised protocol can still send it): down-convert
            // to MIDI 1.0 and reuse the byte-based translation, which honours
            // the dialect the plugin actually asked for
            uint8_t bytes[3];
            if (ClapWrapper::detail::shared::midi2ChannelVoiceToMidi1(&pkt->words[i], bytes) > 0)
            {
              _processAdapter->addMIDIEvent(bytes[0], bytes[1], bytes[2], offset);
            }
          }
        }
        else if (mt == 0x2u)
        {
          // MIDI 1.0 channel voice message packed into one UMP word
          const UInt32 status = (w0 >> 16) & 0xFFu;
          const UInt32 data1 = (w0 >> 8) & 0xFFu;
          const UInt32 data2 = w0 & 0xFFu;
          _processAdapter->addMIDIEvent(status, data1, data2, offset);
        }
        else if (mt == 0x3u)
        {
          // UMP SysEx7 (may span several packets); assembled then delivered
          handleUMPSysEx7(pkt->words[i], (nWords > 1) ? pkt->words[i + 1] : 0u, offset);
        }
        // other message types (utility / system) are not translated
        i += nWords;
      }
      pkt = MIDIEventPacketNext(pkt);
    }
    return noErr;
  }

  // Reassemble a UMP SysEx7 packet stream into a single CLAP SysEx event using the
  // shared reassembler; on a complete message it is already framed with 0xF0/0xF7.
  void handleUMPSysEx7(uint32_t w0, uint32_t w1, UInt32 offset)
  {
    if (!_processAdapter) return;
    if (_sysexReassembler.feed(w0, w1))
    {
      const auto &msg = _sysexReassembler.framedMessage();
      _processAdapter->addSysExEvent(msg.data(), static_cast<uint32_t>(msg.size()), offset);
    }
  }
#endif

  OSStatus SysEx(const UInt8 *inData, UInt32 inLength) override
  {
    if (_processAdapter)
    {
      // the AU SysEx entry point carries no sample offset; deliver at frame 0
      _processAdapter->addSysExEvent(inData, inLength, 0);
    }
    return noErr;
  }

  // Notes (MusicDevice extended-note API)
  OSStatus StartNote(MusicDeviceInstrumentID /*inInstrument*/, MusicDeviceGroupID inGroupID,
                     NoteInstanceID *outNoteInstanceID, UInt32 inOffsetSampleFrame,
                     const MusicDeviceNoteParams &inParams) override
  {
    if (!_processAdapter) return kAudioUnitErr_Uninitialized;

    const int16_t channel = static_cast<int16_t>(inGroupID & 0x0F);
    const int32_t key = static_cast<int32_t>(inParams.mPitch + 0.5f) & 0x7F;  // nearest MIDI key
    // Hand back a unique instance id that also embeds the key in its low 7 bits
    // so StopNote (which only receives the id) can recover it without a table.
    const int32_t note_id = ((_noteInstanceCounter++ & 0x7FFF) << 7) | key;
    *outNoteInstanceID = note_id;

    _processAdapter->startNote(note_id, channel, inParams.mPitch, inParams.mVelocity,
                               inOffsetSampleFrame);
    return noErr;
  }

  OSStatus StopNote(MusicDeviceGroupID inGroupID, NoteInstanceID inNoteInstanceID,
                    UInt32 inOffsetSampleFrame) override
  {
    if (!_processAdapter) return kAudioUnitErr_Uninitialized;
    const int16_t channel = static_cast<int16_t>(inGroupID & 0x0F);
    _processAdapter->stopNote(static_cast<int32_t>(inNoteInstanceID), channel, inOffsetSampleFrame);
    return noErr;
  }

  // unfortunately hidden in the base c++ file
  static void AddNumToDictionary(CFMutableDictionaryRef dict, CFStringRef key, SInt32 value)
  {
    const CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dict, key, num);
    CFRelease(num);
  }

  OSStatus SaveState(CFPropertyListRef *ptPList) override;
  OSStatus RestoreState(CFPropertyListRef plist) override;

  // render
  OSStatus Render(AudioUnitRenderActionFlags &inFlags, const AudioTimeStamp &inTimeStamp,
                  UInt32 inFrames) override;

  OSStatus GetParameterList(AudioUnitScope inScope, AudioUnitParameterID *outParameterList,
                            UInt32 &outNumParameters) override;
  // outParameterList may be a null pointer
  OSStatus GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID,
                            AudioUnitParameterInfo &outParameterInfo) override;

  OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement,
                        AudioUnitParameterValue inValue, UInt32 /*inBufferOffsetInFrames*/) override;

  OSStatus CopyClumpName(AudioUnitScope inScope, UInt32 inClumpID, UInt32 inDesiredNameLength,
                         CFStringRef *outClumpName) override;

  // ---------------- Clap::IHost
  void mark_dirty() override
  {
  }
  void restartPlugin() override
  {
  }
  void request_callback() override
  {
    _requestUICallback = true;
  }

  void setupWrapperSpecifics(const clap_plugin_t *plugin)
      override;  // called when a wrapper could scan for wrapper specific plugins

  void setupAudioBusses(const clap_plugin_t *plugin,
                        const clap_plugin_audio_ports_t *audioports) override final;
  void setupMIDIBusses(const clap_plugin_t *plugin,
                       const clap_plugin_note_ports_t *noteports)
      override final;  // called from initialize() to allow the setup of MIDI ports
  void setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params) override final;

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
    extern bool auv2shared_mm_request_resize(const clap_window_t *, uint32_t, uint32_t);
    return auv2shared_mm_request_resize(_uiconn._window, width, height);
  }
  bool gui_request_show() override
  {
    return false;
  }
  bool gui_request_hide() override
  {
    return false;
  }

  bool register_timer(uint32_t period_ms, clap_id *timer_id) override
  {
    return false;
  }
  bool unregister_timer(clap_id timer_id) override
  {
    return false;
  }

  const char *host_get_name() override
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
      CFStringRef myVersionString = (CFStringRef)CFBundleGetValueForInfoDictionaryKey(
          applicationBundle, CFSTR("CFBundleShortVersionString"));
      if (myVersionString)
      {
        CFStringGetCString(myVersionString, text, 64, kCFStringEncodingUTF8);
        _hostname.append(" ");
        _hostname.append(text);
      }
      else
      {
        myVersionString =
            (CFStringRef)CFBundleGetValueForInfoDictionaryKey(applicationBundle, kCFBundleVersionKey);
        if (myVersionString)
        {
          CFStringGetCString(myVersionString, text, 64, kCFStringEncodingUTF8);
          _hostname.append(" ");
          _hostname.append(text);
        }
      }
      _hostname.append(" (CLAP-as-AUv2)");
    }
    return _hostname.c_str();
  }

  bool track_info_get(clap_track_info_t *info) override
  {
    return false;
  }

  // --------------- IAutomation
  void onBeginEdit(clap_id id) override;
  void onPerformEdit(const clap_event_param_value_t *value) override;
  void onEndEdit(clap_id id) override;

  // --------------- IPlugObject
  void onIdle() override;

  // context menu extension
  bool supportsContextMenu() const override
  {
    return false;
  }
  bool context_menu_populate(const clap_context_menu_target_t *target,
                             const clap_context_menu_builder_t *builder) override
  {
    return false;
  }
  bool context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id) override
  {
    return false;
  }
  bool context_menu_can_popup() override
  {
    return false;
  }
  bool context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index, int32_t x,
                          int32_t y) override
  {
    return false;
  }

  // --------------- IMIDIOutputs
  void send(const Clap::AUv2::clap_multi_event_t &event) override;

 protected:
  void addAudioBusFrom(int bus, const clap_audio_port_info_t *info, bool is_input);

 private:
  // ---------------- glue code stuff
  void addInputBus(int bus, const clap_audio_port_info_t *info);
  void addOutputBus(int bus, const clap_audio_port_info_t *info);

  void activateCLAP();
  void deactivateCLAP();
  bool IsBypassEffect()
  {
    return _isBypassed;
  }
  void SetBypassEffect(bool bypass);

  // --------------- internals

  // the wrapped CLAP:
  std::string _clapname;
  std::string _clapid;
  int _idx;
  os::State _os_attached;

  const clap_plugin_descriptor_t *_desc{nullptr};
  std::shared_ptr<Clap::Plugin> _plugin = nullptr;

  std::unique_ptr<Clap::AUv2::ProcessAdapter> _processAdapter;
  std::atomic<bool> _initialized = false;

  // some info about the wrapped clap
  // audio-port layout captured at PostConstructor. Scanning the CLAP
  // audio-ports extension (count/get) is only legal while the plugin is
  // deactivated, so we snapshot it once and never query it live afterwards.
  struct AudioPortCache
  {
    uint32_t channelCount;
    bool isMain;
  };
  std::vector<AudioPortCache> _inputPortCache;
  std::vector<AudioPortCache> _outputPortCache;

  uint32_t _midi_preferred_dialect = 0;
  uint32_t _midi_supported_dialects = 0;
  bool _midi_wants_midi_input = false;  // takes any input
  bool _midi_understands_midi2 = false;
  // rolling counter feeding the NoteInstanceID/note_id handed out by StartNote
  int32_t _noteInstanceCounter = 0;
#if AUSDK_MIDI2_AVAILABLE
  // reassembles UMP SysEx7 across multi-packet messages on input
  ClapWrapper::detail::shared::SysEx7Reassembler _sysexReassembler;
#endif
  // std::vector<clap_note_port_info_t> _midi_outports_info;

  std::string _hostname = "CLAP-as-AUv2";

#ifdef DUAL_SCHEDULING_ENABLED
  bool _midi_dualscheduling_mode = false;
#endif
  std::map<uint32_t, std::unique_ptr<Clap::AUv2::Parameter>> _parametertree;
  // Between setupParameters() on the main thread and the host's property getters,
  // which arrive on whichever thread the host builds its parameter tree on. Not
  // taken by the audio thread: SetParameter() and the process adapter read the
  // tree under render and must not block. See the note on Clumps above.
  std::mutex _paramTreeMutex;
  std::vector<AudioUnitParameterID> _orderedParameterList;
  bool _paramOrderingProvided{false};
  Clumps _clumps;

  // the CLAP parameter flagged CLAP_PARAM_IS_BYPASS, driven by kAudioUnitProperty_BypassEffect
  clap_id _bypassParamID = CLAP_INVALID_ID;
  bool _isBypassed = false;

  CFStringRef _current_program_name = 0;

  // ------------- for the MIDI output
  AUMIDIOutputCallbackStruct _midioutput_hostcallback = {nullptr, nullptr};
#if AUSDK_MIDI2_AVAILABLE
  // modern MIDI 2.0 / UMP output path (preferred by the host over the callback
  // above). Atomic because a host may install or clear the block while Render is
  // invoking it on the audio thread; replaced blocks are parked in
  // _retiredEventListBlocks instead of being released under the render thread's
  // feet, and drained in deactivateCLAP()/~WrapAsAUV2 when no render can run.
  std::atomic<AUMIDIEventListBlock> _midioutput_hosteventlistblock{nullptr};
  std::vector<AUMIDIEventListBlock> _retiredEventListBlocks;
  MIDIProtocolID _host_midi_protocol = kMIDIProtocol_1_0;
#endif

  std::atomic_bool _requestUICallback = false;

  // the queue from audiothread to UI thread
  ClapWrapper::detail::shared::fixedqueue<queueEvent, 8192> _queueToUI;

  std::vector<std::unique_ptr<MIDIOutput>> _midi_outports;
};

}  // namespace free_audio::auv2_wrapper
