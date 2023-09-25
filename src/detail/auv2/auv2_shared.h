#pragma once

/*
 * This is the base class of all our AU. It is templated on the AU base class and the configuration
 * type (instrument, effect, note effect).
 */

#include "detail/clap/fsutil.h"
#include <iostream>

namespace free_audio::auv2_wrapper
{
template <typename AUBase, bool isInstrument, bool isEffect, bool isNoteEffect>
struct ClapAUv2_Base : public AUBase
{
  std::string _clapname;
  std::string _clapid;
  int _idx;

  Clap::Library _library;

  const clap_plugin_descriptor_t *_desc{nullptr};

  ClapAUv2_Base(const std::string &clapname, const std::string &clapid, int idx,
                AudioComponentInstance ci)
    : AUBase{ci, true}, _clapname(clapname), _clapid(clapid), _idx(idx)
  {
    initialize();
  }

  ClapAUv2_Base(const std::string &clapname, const std::string &clapid, int idx,
                AudioComponentInstance ci, uint32_t inP, uint32_t outP)
    : AUBase{ci, inP, outP}, _clapname(clapname), _clapid(clapid), _idx(idx)
  {
    initialize();
  }

  void initialize()
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
        return;
      }
    }

    if (_clapid.empty())
    {
      if (_idx < 0 || _idx >= (int)_library.plugins.size())
      {
        std::cout << "[ERROR] cannot load by index" << std::endl;
        return;
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
      return;
    }

    std::cout << "[clap-wrapper] auv2: Initialized '" << _desc->id << "' / '" << _desc->name << "'"
              << std::endl;
  }

  bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override
  {
    return true;
  }

  bool CanScheduleParameters() const override
  {
    return false;
  }
};
}  // namespace free_audio::auv2_wrapper
