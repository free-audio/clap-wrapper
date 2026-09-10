#pragma once

/* 

    Copyright (c) 2022 Timo Kaluza (defiantnerd)
                       Paul Walker

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

*/

#include <vector>
#include <functional>
#include <clap/clap.h>
#if WIN
#include <windows.h>
#endif

#include "clapwrapper/vst3.h"
#include "clapwrapper/auv2.h"
#include "clapwrapper/aax.h"
#include "../ara/ara.h"
#include "detail/os/fs.h"

#if MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace Clap
{

std::vector<fs::path> getValidCLAPSearchPaths();
class Plugin;
class IHost;

class Library
{
 public:
  Library();
  ~Library();
  bool load(const fs::path &);

  const clap_plugin_entry_t *_pluginEntry = nullptr;
  const clap_plugin_factory_t *_pluginFactory = nullptr;
  const clap_plugin_factory_as_vst3 *_pluginFactoryVst3Info = nullptr;
  bool _pluginFactoryVst3InfoIsV1 = false;
  const clap_plugin_factory_as_auv2 *_pluginFactoryAUv2Info = nullptr;
  // Optional and independent of the one above. \see CLAP_PLUGIN_FACTORY_INFO_AUV2_LEGACY
  const clap_plugin_factory_auv2_legacy *_pluginFactoryAUv2Legacy = nullptr;
  const clap_plugin_factory_as_aax_t *_pluginFactoryAAXInfo = nullptr;
  const clap_ara_factory_t *_pluginFactoryARAInfo = nullptr;
  // The plugin's CLAP preset-discovery factory, when it has one. Unlike the
  // *_Info factories above this is not wrapper metadata: it is the plugin
  // telling a host where its presets live and what they contain. No VST3 or
  // AUv2 host will ever ask for it, so the wrapper has to be the indexer
  // itself - see detail/clap/preset_discovery.h.
  const clap_preset_discovery_factory_t *_pluginFactoryPresetDiscovery = nullptr;
  std::vector<const clap_plugin_descriptor_t *> plugins;

  const clap_plugin_info_as_vst3_t *get_vst3_info(uint32_t index) const;
  const char *get_vst3_compatibility() const;
  const clap_plugin_info_as_aax_t *get_aax_info(uint32_t index) const;

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

  // Wire up a statically-linked clap_entry. Intended for targets where the
  // CLAP's clap_entry global is in the same binary as the wrapper (iOS
  // AUv3, clap-first standalones). The caller supplies the entry pointer
  // directly, bypassing any filesystem / dlopen search. Safe to call once
  // after construction.
  void useStaticEntry(const clap_plugin_entry_t *entry, const char *path);

 private:
#if MAC
  CFBundleRef _bundle{nullptr};
#endif

#if LIN
  void *_handle{nullptr};
#endif

#if WIN
  HMODULE _handle = 0;
  bool getEntryFunction(HMODULE handle, const char *path);
#endif

  void setupPluginsFromPluginEntry(const char *p);
  bool _selfcontained = false;
};

}  // namespace Clap
