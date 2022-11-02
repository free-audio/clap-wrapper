#pragma once

#include <vector>
#include <filesystem>
#include <functional>
#include <clap/clap.h>
#if WIN
#include <Windows.h>
#endif

#include "clapwrapper/vst3.h"

#if MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace Clap
{

  std::vector<std::filesystem::path> getValidCLAPSearchPaths();
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
    std::vector<const clap_plugin_descriptor_t*> plugins;
    const clap_plugin_info_as_vst3_t* get_vst3_info(uint32_t index);

    bool hasEntryPoint() const {
#if WIN
        return _handle != 0 || _selfcontained;
#endif

#if MAC
      return _bundle != 0 || _selfcontained;
#endif

#if LIN
        return _handle != nullptr;
#endif

    }

  private:
#if MAC
    // FIXME keep a bundle ref around
   CFBundleRef _bundle{nullptr};
#endif

#if LIN
    void *_handle{nullptr};
#endif


#if WIN
    HMODULE _handle = 0;
    bool getEntryFunction(HMODULE handle, const char* path);
#endif

    void setupPluginsFromPluginEntry(const char* p);
    bool _selfcontained = false;
  };

}


