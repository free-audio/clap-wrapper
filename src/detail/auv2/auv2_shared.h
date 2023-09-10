
#pragma once

#include "detail/clap/fsutil.h"
#include <iostream>

namespace free_audio::auv2_wrapper {
struct ClapBridge {
  std::string _clapname;
  std::string _clapid;
  int _idx;

  Clap::Library _library;

  const clap_plugin_descriptor_t *_desc{nullptr};

  ClapBridge(const std::string &clapname, const std::string &clapid, int idx)
      : _clapname(clapname), _clapid(clapid), _idx(idx) {
    std::cout << "[clap-wraper] auv2: creating clap bridge nm=" << clapname
              << " id=" << clapid << " idx=" << idx << std::endl;
  }

  void initialize() {
    if (!_library.hasEntryPoint()) {
      if (_clapname.empty()) {
        std::cout << "[ERROR] _clapname (" << _clapname
                  << ") empty and no internal entry point" << std::endl;
      }
      bool loaded{false};
      auto csp = Clap::getValidCLAPSearchPaths();
      for (auto &cs : csp) {
        if (fs::is_directory(cs / (_clapname + ".clap")))
          if (_library.load(_clapname.c_str())) {
            std::cout << "[clap-wrapper] auv2 loaded clap from "
                      << cs.u8string() << std::endl;
            loaded = true;
            break;
          }
      }
      if (!loaded) {
        std::cout << "[ERROR] cannot load clap" << std::endl;
        return;
      }
    }

    if (_clapid.empty()) {
      if (_idx < 0 || _idx >= _library.plugins.size()) {
        std::cout << "[ERROR] cannot load by index" << std::endl;
        return;
      }
      _desc = _library.plugins[_idx];
    } else {
      for (auto *d : _library.plugins) {
        if (strcmp(d->id, _clapid.c_str()) == 0) {
          _desc = d;
        }
      }
    }

    if (!_desc) {
      std::cout << "[ERROR] cannot determine plugin description" << std::endl;
      return;
    }

    std::cout << "[clap-wrapper] auv2: Initialized '" << _desc->id << "' / '"
              << _desc->name << "'" << std::endl;
  }
};
} // namespace free_audio::auv2_wrapper