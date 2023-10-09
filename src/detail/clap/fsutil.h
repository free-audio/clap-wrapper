#pragma once

/* 

    Copyright (c) 2022 Timo Kaluza (defiantnerd)
                       Paul Walker

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

*/

#if MAC
#if MACOS_USE_STD_FILESYSTEM
#include <filesystem>
namespace fs = std::filesystem;
#else
#include "ghc/filesystem.hpp"
namespace fs = ghc::filesystem;
#endif
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

#include <vector>
#include <functional>
#include <clap/clap.h>
#if WIN
#include <Windows.h>
#endif

#include "clapwrapper/vst3.h"
#include "clapwrapper/auv2.h"

#if MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace Clap
{

std::vector<fs::path> getValidCLAPSearchPaths();
std::string toString(const CFStringRef aString );
class Plugin;
class IHost;

class Library
{
 public:
  Library();
  ~Library();
  bool load(const char* name);

  const clap_plugin_entry_t* _pluginEntry = nullptr;
  const clap_plugin_factory_t* _pluginFactory = nullptr;
  const clap_plugin_factory_as_vst3* _pluginFactoryVst3Info = nullptr;
  const clap_plugin_factory_as_auv2* _pluginFactoryAUv2Info = nullptr;
  std::vector<const clap_plugin_descriptor_t*> plugins;
  const clap_plugin_info_as_vst3_t* get_vst3_info(uint32_t index) const;

#if MAC
  CFBundleRef getBundleRef()
  {
    return _bundle;
  }
#endif
  
  bool hasEntryPoint() const
  {
#if WIN
    return _handle != 0 || _selfcontained;
#endif

#if MAC
    return _bundle != nullptr || _selfcontained;
#endif

#if LIN
    return _handle != nullptr || _selfcontained;
#endif
  }

 private:
#if MAC
  CFBundleRef _bundle{nullptr};
#endif

#if LIN
  void* _handle{nullptr};
#endif

#if WIN
  HMODULE _handle = 0;
  bool getEntryFunction(HMODULE handle, const char* path);
#endif

  void setupPluginsFromPluginEntry(const char* p);
  bool _selfcontained = false;
};

}  // namespace Clap
