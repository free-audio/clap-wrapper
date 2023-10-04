#pragma once

/*
 * This is the base class of all our AU. It is templated on the AU base class and the configuration
 * type (instrument, effect, note effect).
 */

#include "detail/clap/fsutil.h"
#include <iostream>

#include "detail/os/osutil.h"
#include "clap_proxy.h"

#include <AudioToolbox/AudioUnitProperties.h>
#include <CoreMIDI/MIDIServices.h>
#define CWAUTRACE                                                                               \
  std::cout << "[clap-wrapper auv2 trace] " << __func__ << " @ auv2_shared.h line " << __LINE__ \
            << std::endl

namespace free_audio::auv2_wrapper
{

Clap::Library _library; // holds the library

template <typename AUBase_t>
struct ClapAUv2_Base : public AUBase_t
  , Clap::IHost
{
  static constexpr bool isInstrument{std::is_same_v<AUBase_t, ausdk::MusicDeviceBase>};
  static constexpr bool isEffect{std::is_same_v<AUBase_t, ausdk::AUEffectBase>};
  static constexpr bool isNoteEffect{std::is_same_v<AUBase_t, ausdk::AUMIDIEffectBase>};

  static_assert(isInstrument + isEffect + isNoteEffect == 1,
                "You must be one and only one of instrument, effect, or note effect");

  std::string _clapname;
  std::string _clapid;
  int _idx;

  std::shared_ptr<Clap::Plugin> _plugin = nullptr;

  const clap_plugin_descriptor_t *_desc{nullptr};

  ClapAUv2_Base(const std::string &clapname, const std::string &clapid, int idx,
                AudioComponentInstance ci)
    : AUBase_t{ci, true}, _clapname(clapname), _clapid(clapid), _idx(idx)
  {
      
  }

  ClapAUv2_Base(const std::string &clapname, const std::string &clapid, int idx,
                AudioComponentInstance ci, uint32_t inP, uint32_t outP)
    : AUBase_t{ci, inP, outP}, _clapname(clapname), _clapid(clapid), _idx(idx)
  {
  }

  bool initializeClapDesc()
  {
    if constexpr (isEffect)
    {
      std::cout << "[clap-wrapper] auv2: creating audio effect" << std::endl;
    }
    else if constexpr (isInstrument)
    {
      std::cout << "[clap-wrapper] auv2: creating instrument" << std::endl;
    }
    else if constexpr (isNoteEffect)
    {
      std::cout << "[clap-wrapper] auv2: creating note effect" << std::endl;
    }

    std::cout << "[clap-wrapper] auv2: id='" << _clapid << "' index=" << _idx << std::endl;

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
                               return fs::is_directory(fp) && _library.load(fp.u8string().c_str());
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

  OSStatus Initialize() override
  {
    if (!_desc)
    {
      if (!initializeClapDesc())
      {
        return 1;
      }
      else
      {
        std::cout << "[clap-wrapper] auv2: Initialized '" << _desc->id << "' / '" << _desc->name
                  << "' / '" << _desc->version << "'" << std::endl;
      }
    }
    if (!_desc) return 2;

    
    auto res = AUBase_t::Initialize();
    if (res != noErr) return res;

    
    /*
     * ToDo: Stand up the host, create the plugin instance here
     */
    _plugin = Clap::Plugin::createInstance(_library._pluginFactory, _desc->id, this);

    // initialize will call IHost functions to set up busses etc, so be ready
    _plugin->initialize();
    

    return noErr;
  }

  OSStatus Start() override
  {
    return AUBase_t::Start();
  }
  
  void Cleanup() override
  {
    // TODO: Destroy the plugin etc
    _plugin->terminate();
    _plugin.reset();
    
    AUBase_t::Cleanup();
  }
  
  bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override
  {
    return true;
  }

  bool CanScheduleParameters() const override
  {
    return true;
  }

  // properties
  
  OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope,
    AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable) override
  {
    auto result =  AUBase_t::GetPropertyInfo(inID,inScope,inElement,outDataSize,outWritable);

    return result;
  }
  
  OSStatus GetProperty(
                       AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, void* outData) override
  {
    auto result = AUBase_t::GetProperty(inID,inScope,inElement,outData);

    return result;
  }
  
  OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope,
    AudioUnitElement inElement, const void* inData, UInt32 inDataSize) override
  {
    OSStatus result = noErr;
    switch(inID)
    {
      case kAudioUnitProperty_AUHostIdentifier:
        {
          const auto m = static_cast<const AUHostIdentifier*>(inData);
          printf("[auv2-wrapper] Hostname: %s\n",Clap::toString( m->hostName).c_str());
        }
        break;
      default:
        result = AUBase_t::SetProperty(inID,inScope,inElement,inData,inDataSize);
    }
    
    return result;
  }
  
  // -----------
  
  OSStatus MIDIEvent(
    UInt32 inStatus, UInt32 inData1, UInt32 inData2, UInt32 inOffsetSampleFrame) override
  {
    return AUBase_t::MIDIEvent(inStatus, inData1, inData2, inOffsetSampleFrame);
  }
  
  OSStatus StartNote(MusicDeviceInstrumentID /*inInstrument*/,
    MusicDeviceGroupID /*inGroupID*/, NoteInstanceID* outNoteInstanceID,
                     UInt32 /*inOffsetSampleFrame*/, const MusicDeviceNoteParams& inParams) override
  {
    if ( outNoteInstanceID)   *outNoteInstanceID = 12;
    return noErr;
  }
  
  OSStatus SysEx(const UInt8* inData, UInt32 inLength) override
  {
    return AUBase_t::SysEx(inData, inLength);
  }
  
  OSStatus GetParameterList(AudioUnitScope inScope, AudioUnitParameterID *outParameterList,
                            UInt32 &outNumParameters) override
  {
    CWAUTRACE;
    return AUBase_t::GetParameterList(inScope, outParameterList, outNumParameters);
  }
  // outParameterList may be a null pointer
  OSStatus GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID,
                            AudioUnitParameterInfo &outParameterInfo) override
  {
    CWAUTRACE;
    return AUBase_t::GetParameterInfo(inScope, inParameterID, outParameterInfo);
  }
  
  // ---------------- Clap::IHost
  void mark_dirty() override {}
  void restartPlugin() override {}
  void request_callback() override {}

  void setupWrapperSpecifics(const clap_plugin_t* plugin) override; // called when a wrapper could scan for wrapper specific plugins

  void setupAudioBusses(
      const clap_plugin_t* plugin,
      const clap_plugin_audio_ports_t*
          audioports) override {
            auto numAudioInputs = audioports->count(plugin, true);
            auto numAudioOutputs = audioports->count(plugin, false);

            fprintf(stderr, "\tAUDIO in: %d, out: %d\n", (int)numAudioInputs, (int)numAudioOutputs);

            ausdk::AUBase::GetScope(kAudioUnitScope_Input).SetNumberOfElements(numAudioInputs);
            
            for ( decltype(numAudioInputs) i = 0 ; i < numAudioInputs ; ++i)
            {
              clap_audio_port_info_t info;
              if (audioports->get(plugin, i, true, &info))
              {
                // Inputs();
                // addAudioBusFrom(&info, true);
              }
            }
            
            ausdk::AUBase::GetScope(kAudioUnitScope_Output).SetNumberOfElements(numAudioOutputs);
            
            ausdk::AUBase::ReallocateBuffers();
            
          }  // called from initialize() to allow the setup of audio ports
  void setupMIDIBusses(
      const clap_plugin_t* plugin,
      const clap_plugin_note_ports_t*
          noteports) override {}  // called from initialize() to allow the setup of MIDI ports
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


};
}  // namespace free_audio::auv2_wrapper

#undef CWAUTRACE
