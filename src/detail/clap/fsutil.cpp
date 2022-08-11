#include "fsutil.h"

#if WIN
#include <Windows.h>
#endif

#if MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

#if LIN
#include <dlfcn.h>
#endif


namespace Clap
{
  std::vector<std::filesystem::path> getValidCLAPSearchPaths()
  {
    std::vector<std::filesystem::path> res;

#if MAC
    extern std::vector<std::filesystem::path> getMacCLAPSearchPaths();
    res = getMacCLAPSearchPaths();
    // getSystemPaths(res);
#endif

#if LIN
    res.emplace_back("/usr/lib/clap");
    res.emplace_back(std::filesystem::path(getenv("HOME")) / std::filesystem::path(".clap"));
#endif

#if WIN
    {
      // I think this should use SHGetKnownFilderLocation but I don't know windows well enough
      auto p = getenv("COMMONPROGRAMFILES");
      if (p)
      {
        res.emplace_back(std::filesystem::path{ p } / "CLAP");
      }
      auto q = getenv("LOCALAPPDATA");
      if (q)
      {
        res.emplace_back(std::filesystem::path{ q } / "Programs" / "Common" / "CLAP");
      }
    }
#endif

    auto p = getenv("CLAP_PATH");

    if (p)
    {
#if WIN
      auto sep = ';';
#else
      auto sep = ':';
#endif
      auto cp = std::string(p);

      size_t pos;
      while ((pos = cp.find(sep)) != std::string::npos)
      {
        auto item = cp.substr(0, pos);
        cp = cp.substr(pos + 1);
        res.emplace_back(std::filesystem::path{ item });
      }
      if (cp.size())
        res.emplace_back(std::filesystem::path{ cp });
    }

    return res;
  }

  bool Library::load(const char* name)
  {
#if MAC
     _pluginEntry = nullptr;

     auto cs = CFStringCreateWithBytes(kCFAllocatorDefault, (uint8_t *)name, strlen(name),
                                       kCFStringEncodingUTF8, false);
     auto bundleURL =
             CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cs, kCFURLPOSIXPathStyle, true);

     auto bundle = CFBundleCreate(kCFAllocatorDefault, bundleURL);

     auto db = CFBundleGetDataPointerForName(bundle, CFSTR("clap_entry"));

     CFRelease(bundle);
     CFRelease(bundleURL);
     CFRelease(cs);

     _pluginEntry = (const clap_plugin_entry *)db;

     setupPluginsFromPluginEntry(name);
     return _pluginEntry != nullptr;
#endif

#if WIN
    if (_handle && !_selfcontained)
    {
      FreeLibrary(_handle);
    }
    _handle = 0;
    _pluginEntry = nullptr;
    _handle = LoadLibraryA(name);
    if (_handle)
    {
      if (!getEntryFunction(_handle, name))
      {
        FreeLibrary(_handle);
        _handle = NULL;
      }
    }

    setupPluginsFromPluginEntry(name);
    return _handle != 0;
#endif

#if LIN
      int *iptr;

      _handle = dlopen(name, RTLD_LOCAL | RTLD_LAZY);
      if (!_handle)
          return false;

      iptr = (int *)dlsym(_handle, "clap_entry");
      if (!iptr)
          return false;

      _pluginEntry = (const clap_plugin_entry_t *)iptr;
      setupPluginsFromPluginEntry(name);
      return true;
#endif
  }

#if 0
  std::shared_ptr<Plugin> Library::createInstance(IHost* host, size_t index)
  {
    if (plugins.size() > index)
    {
      auto plug = std::shared_ptr<Plugin>(new Plugin(host));
      auto instance = this->_pluginFactory->create_plugin(this->_pluginFactory, plug->getClapHostInterface(), plugins[index]->id);
      plug->connectClap(instance);

      return plug;
    }
    return nullptr;
  }

#endif 

#if WIN
  bool Library::getEntryFunction(HMODULE handle, const char* path)
  {
    if (handle)
    {
      _pluginEntry = reinterpret_cast<const clap_plugin_entry*>(GetProcAddress(handle, "clap_entry"));
      if (_pluginEntry)
      {
         setupPluginsFromPluginEntry(path);
      }
    }
    return (_pluginEntry && !plugins.empty());
  }
#endif

  void Library::setupPluginsFromPluginEntry(const char* path) {
     if (clap_version_is_compatible(_pluginEntry->clap_version)) {
        if (_pluginEntry->init(path)) {
           _pluginFactory =
                   static_cast<const clap_plugin_factory *>(_pluginEntry->get_factory(CLAP_PLUGIN_FACTORY_ID));

           auto count = _pluginFactory->get_plugin_count(_pluginFactory);

           for (decltype(count) i = 0; i < count; ++i) {
              auto desc = _pluginFactory->get_plugin_descriptor(_pluginFactory, i);
              if (clap_version_is_compatible(desc->clap_version)) {
                 plugins.push_back(desc);
              } else {
                 // incompatible
              }
           }
        }
     }
     return true;
  }
  static void ffeomwe()
  {}

  Library::Library()
  {
#if WIN
    static TCHAR modulename[2048];
    HMODULE selfmodule;
    if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)ffeomwe, &selfmodule))
    {
      auto size = GetModuleFileName(selfmodule, modulename, 2048);
    }
    if (selfmodule)
    {
      if (this->getEntryFunction(selfmodule, (const char*)modulename))
      {
        _selfcontained = true;
      }
    }
#endif
  }

  Library::~Library()
  {
    if (_pluginEntry)
    {
      _pluginEntry->deinit();
    }
#if MAC
    // FIXME keep the bundle ref and free it here
#endif

#if LIN
    if (_handle)
    {
        dlclose(_handle);
        _handle = nullptr;
    }
#endif

#if WIN
    if (_handle && !_selfcontained)
    {
      FreeLibrary(_handle);
    }
#endif
  }

}