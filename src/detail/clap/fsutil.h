#pragma once

#include <vector>
#include <filesystem>
#include <functional>
#include <clap/clap.h>
#if WIN
#include <Windows.h>
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
    // bool isSelfContained() const { return _selfcontained; }
    bool load(const char* name);
    bool hasEntryPoint() const { return _handle != 0 || _selfcontained; }
    // bool isLoaded() const { return _handle != 0; }
    
    const clap_plugin_entry* _pluginEntry = nullptr;
    const clap_plugin_factory* _pluginFactory = nullptr;
    std::vector<const clap_plugin_descriptor_t*> plugins;
  private:
#if MAC
      int64_t _handle{0};
#endif

#if WIN
    HMODULE _handle = 0;
    bool getEntryFunction(HMODULE handle, const char* path);
#endif

    void setupPluginsFromPluginEntry(const char* p);
    bool _selfcontained = false;
  };

}


