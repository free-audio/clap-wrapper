/*

    Copyright (c) 2022 Timo Kaluza (defiantnerd)
                       Paul Walker

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

*/

#include "fsutil.h"
#include <cassert>
#if WIN
#include <Windows.h>
#endif

#if MAC
#include <CoreFoundation/CoreFoundation.h>
#include <iostream>
#endif

#if LIN
#include <dlfcn.h>
#include <iostream>
#endif


namespace Clap
{
#if WIN
  std::string getEnvVariable(const char* varname)
  {
    char* val;
    size_t len;
    auto err = _dupenv_s(&val, &len, varname);
    if (err) return std::string();
    if (val == nullptr) return std::string();
    std::string result(val);
    free(val);
    return result;      
  }
#endif

  std::vector<fs::path> getValidCLAPSearchPaths()
  {
    std::vector<fs::path> res;

#if MAC
    extern std::vector<fs::path> getMacCLAPSearchPaths();
    res = getMacCLAPSearchPaths();
    // getSystemPaths(res);
#endif

#if LIN
    res.emplace_back("/usr/lib/clap");
    res.emplace_back(fs::path(getenv("HOME")) / fs::path(".clap"));
#endif

#if WIN
    {
      // I think this should use SHGetKnownFilderLocation but I don't know windows well enough
      auto p = getEnvVariable("COMMONPROGRAMFILES");
      if (!p.empty())
      {
        res.emplace_back(fs::path{ p } / "CLAP");
      }
      auto q = getEnvVariable("LOCALAPPDATA");
      if (!q.empty())
      {
        res.emplace_back(fs::path{ q } / "Programs" / "Common" / "CLAP");
      }
    }
    auto cp = getEnvVariable("CLAP_PATH");
    auto sep = ';';
#else
    std::string cp;
    auto p = getenv("CLAP_PATH");
    if (p)
    {
      cp = std::string(p);
    }
    auto sep = ':';
#endif

    if (cp.empty())
    {
      size_t pos;
      while ((pos = cp.find(sep)) != std::string::npos)
      {
        auto item = cp.substr(0, pos);
        cp = cp.substr(pos + 1);
        res.emplace_back(fs::path{ item });
      }
      if (cp.size())
        res.emplace_back(fs::path{ cp });
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

     _bundle = CFBundleCreate(kCFAllocatorDefault, bundleURL);
     CFRelease(bundleURL);
     CFRelease(cs);

     if (!_bundle) {
        return false;
     }

     auto db = CFBundleGetDataPointerForName(_bundle, CFSTR("clap_entry"));

     _pluginEntry = (const clap_plugin_entry *)db;

     setupPluginsFromPluginEntry(name);
     return _pluginEntry != nullptr;
#endif

#if WIN
    if (_handle && !_selfcontained)
    {
      if (_pluginEntry)
      {
        _pluginEntry->deinit();
      }
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

  const clap_plugin_info_as_vst3_t* Library::get_vst3_info(uint32_t index)
  {
    if (_pluginFactoryVst3Info && _pluginFactoryVst3Info->get_vst3_info)
    {
      return _pluginFactoryVst3Info->get_vst3_info(_pluginFactoryVst3Info, index);
    }
    return nullptr;
  }

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
           _pluginFactoryVst3Info =
                  static_cast<const clap_plugin_factory_as_vst3*>(_pluginEntry->get_factory(CLAP_PLUGIN_FACTORY_INFO_VST3));

           // detect plugins that do not check the CLAP_PLUGIN_FACTORY_ID
           if ((void*)_pluginFactory == (void*)_pluginFactoryVst3Info)
           {
             _pluginFactoryVst3Info = nullptr;
           }

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
      (void)size;
    }
    if (selfmodule)
    {
      if (this->getEntryFunction(selfmodule, (const char*)modulename))
      {
        _selfcontained = true;
      }
    }
#endif

#if LIN
    Dl_info info;
    if (dladdr(reinterpret_cast<const void *>(&ffeomwe), &info) &&
        info.dli_fname[0])
    {
      auto lhandle = dlopen(info.dli_fname, RTLD_LOCAL | RTLD_LAZY);
      if (lhandle)
      {
        auto liptr = (int *)dlsym(_handle, "clap_entry");
        if (liptr)
        {
              _handle = lhandle; // as a result the Library dtor will dlclose me
              _pluginEntry = (const clap_plugin_entry_t *)liptr;
              _selfcontained = true;

              setupPluginsFromPluginEntry(info.dli_fname);
        }
      }
    }
#endif

#if MAC
    extern fs::path sharedLibraryBundlePath();
    auto selfp = sharedLibraryBundlePath();
    if (!selfp.empty())
    {
      std::string name = selfp.u8string();
      CFURLRef bundleUrl = CFURLCreateFromFileSystemRepresentation (0,
                                                                   (const unsigned char*)name.c_str (),
                                                                   name.size(), true);
      if (bundleUrl)
      {
        auto pluginBundle = CFBundleCreate(0, bundleUrl);
        CFRelease(bundleUrl);

        if (pluginBundle)
        {
          auto db = CFBundleGetDataPointerForName(pluginBundle, CFSTR("clap_entry"));
          if (db)
          {
             _bundle = pluginBundle;
             _pluginEntry = (const clap_plugin_entry_t *)db;
             _selfcontained = true;

             setupPluginsFromPluginEntry(selfp.u8string().c_str());
          }
          else
          {
             CFRelease(pluginBundle);
          }
        }
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
    if (_bundle)
    {
      CFRelease(_bundle);
      _bundle = nullptr;
    }
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
