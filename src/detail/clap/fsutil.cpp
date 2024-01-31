/*

    Copyright (c) 2022 Timo Kaluza (defiantnerd)
                       Paul Walker

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

*/

#include "fsutil.h"
#include <cassert>
#if WIN
#include <windows.h>
#include <shlobj.h>
#include <sstream>
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
fs::path get_known_folder(const KNOWNFOLDERID &id)
{
  wchar_t *buffer{nullptr};

  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &buffer)))
  {
    fs::path data{std::wstring(buffer)};
    CoTaskMemFree(buffer);

    return data;
  }

  else
    return {};
}

std::vector<std::wstring> split_clap_path(const std::wstring &in)
{
  std::vector<std::wstring> res;
  std::wstringstream ss(in);
  std::wstring item;

  while (std::getline(ss, item, L';')) res.push_back(item);

  return res;
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
  auto p{get_known_folder(FOLDERID_ProgramFilesCommon)};
  if (fs::exists(p)) res.emplace_back(p / "CLAP");

  auto q{get_known_folder(FOLDERID_LocalAppData)};
  if (fs::exists(q)) res.emplace_back(q / "Programs" / "Common" / "CLAP");

  auto len{::GetEnvironmentVariableW(L"CLAP_PATH", nullptr, 0)};

  if (len > 0)
  {
    std::wstring cp;
    cp.resize(len);
    cp.resize(::GetEnvironmentVariableW(L"CLAP_PATH", &cp[0], len));
    auto paths{split_clap_path(cp)};

    for (const auto &path : paths)
    {
      if (fs::exists(path)) res.emplace_back(path);
    }
  }
#else
  std::string cp;
  auto p = getenv("CLAP_PATH");
  if (p)
  {
    cp = std::string(p);
  }
  auto sep = ':';

  if (cp.empty())
  {
    size_t pos;
    while ((pos = cp.find(sep)) != std::string::npos)
    {
      auto item = cp.substr(0, pos);
      cp = cp.substr(pos + 1);
      res.emplace_back(item);
    }
    if (!cp.empty()) res.emplace_back(cp);
  }
#endif

  auto paths{res};
  for (const auto &path : paths)
  {
    try
    {
      for (auto subdirectory :
           fs::recursive_directory_iterator(path, fs::directory_options::follow_directory_symlink |
                                                      fs::directory_options::skip_permission_denied))
      {
        try
        {
          if (subdirectory.is_directory()) res.emplace_back(subdirectory.path());
        }
        catch (const fs::filesystem_error &e)
        {
        }
      }
    }
    catch (const fs::filesystem_error &e)
    {
    }
  }

  return res;
}

bool Library::load(const fs::path &path)
{
#if MAC
  _pluginEntry = nullptr;

  auto u8path = path.u8string();
  auto cs = CFStringCreateWithBytes(kCFAllocatorDefault, (uint8_t *)u8path.data(), u8path.size(),
                                    kCFStringEncodingUTF8, false);
  auto bundleURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cs, kCFURLPOSIXPathStyle, true);

  _bundle = CFBundleCreate(kCFAllocatorDefault, bundleURL);
  CFRelease(bundleURL);
  CFRelease(cs);

  if (!_bundle)
  {
    return false;
  }

  auto db = CFBundleGetDataPointerForName(_bundle, CFSTR("clap_entry"));

  _pluginEntry = (const clap_plugin_entry *)db;

  setupPluginsFromPluginEntry(u8path.c_str());
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
  _handle = nullptr;
  _pluginEntry = nullptr;
  _handle = LoadLibraryW(path.native().c_str());
  if (_handle)
  {
    if (!getEntryFunction(_handle, path.u8string().c_str()))
    {
      FreeLibrary(_handle);
      _handle = nullptr;
    }
  }

  return _handle != nullptr;
#endif

#if LIN
  int *iptr;

  auto u8path = path.u8string();
  _handle = dlopen(u8path.c_str(), RTLD_LOCAL | RTLD_LAZY);
  if (!_handle) return false;

  iptr = (int *)dlsym(_handle, "clap_entry");
  if (!iptr) return false;

  _pluginEntry = (const clap_plugin_entry_t *)iptr;
  setupPluginsFromPluginEntry(u8path.c_str());
  return true;
#endif
}

const clap_plugin_info_as_vst3_t *Library::get_vst3_info(uint32_t index) const
{
  if (_pluginFactoryVst3Info && _pluginFactoryVst3Info->get_vst3_info)
  {
    return _pluginFactoryVst3Info->get_vst3_info(_pluginFactoryVst3Info, index);
  }
  return nullptr;
}

#if WIN
bool Library::getEntryFunction(HMODULE handle, const char *path)
{
  if (handle)
  {
    _pluginEntry = reinterpret_cast<const clap_plugin_entry *>(GetProcAddress(handle, "clap_entry"));
    if (_pluginEntry)
    {
      setupPluginsFromPluginEntry(path);
    }
  }
  return (_pluginEntry && !plugins.empty());
}
#endif

void Library::setupPluginsFromPluginEntry(const char *path)
{
  if (clap_version_is_compatible(_pluginEntry->clap_version))
  {
    if (_pluginEntry->init(path))
    {
      _pluginFactory =
          static_cast<const clap_plugin_factory *>(_pluginEntry->get_factory(CLAP_PLUGIN_FACTORY_ID));
      _pluginFactoryVst3Info = static_cast<const clap_plugin_factory_as_vst3 *>(
          _pluginEntry->get_factory(CLAP_PLUGIN_FACTORY_INFO_VST3));
      _pluginFactoryAUv2Info = static_cast<const clap_plugin_factory_as_auv2 *>(
          _pluginEntry->get_factory(CLAP_PLUGIN_FACTORY_INFO_AUV2));

      // detect plugins that do not check the CLAP_PLUGIN_FACTORY_ID
      if ((void *)_pluginFactory == (void *)_pluginFactoryVst3Info)
      {
        _pluginFactoryVst3Info = nullptr;
        _pluginFactoryAUv2Info = nullptr;
      }

      auto count = _pluginFactory->get_plugin_count(_pluginFactory);

      for (decltype(count) i = 0; i < count; ++i)
      {
        auto desc = _pluginFactory->get_plugin_descriptor(_pluginFactory, i);
        if (clap_version_is_compatible(desc->clap_version))
        {
          plugins.push_back(desc);
        }
        else
        {
          // incompatible
        }
      }
    }
  }
}

#if WIN || LIN
// This is a small stub to resolve the self dll. MacOs uses a different approach
// in sharedLibraryBundlePath
static void ffeomwe()
{
}
#endif

Library::Library()
{
#if WIN
  fs::path modulename;
  HMODULE selfmodule;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)ffeomwe, &selfmodule))
  {
    std::wstring p;
    auto size = GetModuleFileNameW(selfmodule, nullptr, 0);
    p.resize(size);
    size = GetModuleFileNameW(selfmodule, &p[0], size);
    modulename = std::move(p);
  }
  if (selfmodule)
  {
    if (this->getEntryFunction(selfmodule, modulename.u8string().c_str()))
    {
      _selfcontained = true;
    }
  }
#endif

#if LIN
  Dl_info info;
  if (dladdr(reinterpret_cast<const void *>(&ffeomwe), &info) && info.dli_fname[0])
  {
    auto lhandle = dlopen(info.dli_fname, RTLD_LOCAL | RTLD_LAZY);
    if (lhandle)
    {
      auto liptr = (int *)dlsym(_handle, "clap_entry");
      if (liptr)
      {
        _handle = lhandle;  // as a result the Library dtor will dlclose me
        _pluginEntry = (const clap_plugin_entry_t *)liptr;
        _selfcontained = true;

        setupPluginsFromPluginEntry(info.dli_fname);
      }
    }
  }
#endif

#if MAC
  extern std::string toString(const CFStringRef aString);
  extern fs::path sharedLibraryBundlePath();
  auto selfp = sharedLibraryBundlePath();
  if (!selfp.empty())
  {
    std::string name = selfp.u8string();
    CFURLRef bundleUrl = CFURLCreateFromFileSystemRepresentation(
        nullptr, (const unsigned char *)name.c_str(), name.size(), true);
    if (bundleUrl)
    {
      auto pluginBundle = CFBundleCreate(nullptr, bundleUrl);
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

}  // namespace Clap
