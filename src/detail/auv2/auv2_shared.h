#pragma once

/*
 * This is the base class of all our AU. It is templated on the AU base class and the configuration
 * type (instrument, effect, note effect).
 */

#include "detail/clap/fsutil.h"
#include <iostream>

#define CWAUTRACE                                                                               \
  std::cout << "[clap-wrapper auv2 trace] " << __func__ << " @ " << __FILE__ << ":" << __LINE__ \
            << std::endl

namespace free_audio::auv2_wrapper
{
template <typename AUBase_t>
struct ClapAUv2_Base : public AUBase_t
{
  static constexpr bool isInstrument{std::is_same_v<AUBase_t, ausdk::MusicDeviceBase>};
  static constexpr bool isEffect{std::is_same_v<AUBase_t, ausdk::AUEffectBase>};
  static constexpr bool isNoteEffect{std::is_same_v<AUBase_t, ausdk::AUMIDIEffectBase>};

  static_assert(isInstrument + isEffect + isNoteEffect == 1,
                "You must be one and only one of instrument, effect, or note effect");

  std::string _clapname;
  std::string _clapid;
  int _idx;

  Clap::Library _library;

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

    /*
     * ToDo: Stand up the host, create the plugin instance here
     */

    auto res = AUBase_t::Initialize();
    if (res != noErr) return res;

    return noErr;
  }

  void Cleanup() override
  {
    // TODO: Destroy the plugin etc
    AUBase_t::Cleanup();
  }

  bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override
  {
    return true;
  }

  bool CanScheduleParameters() const override
  {
    return false;
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
};
}  // namespace free_audio::auv2_wrapper

#undef CWAUTRACE
