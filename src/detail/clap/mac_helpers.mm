/* 
    Copyright (c) 2022 Paul Walker
                       Timo Kaluza (defiantnerd)
                       

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/defiantnerd/clap-wrapper for full license details.

*/

#include <vector>
#include <dlfcn.h>

// No need to ifdef this - it is mac only
#if MACOS_USE_STD_FILESYSTEM
#include <filesystem>
namespace fs = std::filesystem;
#else
#include "ghc/filesystem.hpp"
namespace fs = ghc::filesystem;
#endif

#include <Foundation/Foundation.h>

namespace Clap
{
fs::path sharedLibraryBundlePath()
{
  Dl_info info;
  if (!dladdr(reinterpret_cast<const void *>(&sharedLibraryBundlePath), &info) || !info.dli_fname[0])
  {
    // If dladdr(3) returns zero, dlerror(3) won't know why either
    return {};
  }
  try
  {
    auto res = fs::path{info.dli_fname};
    res = res.parent_path().parent_path();  // this might throw if not in bundle so catch
    if (res.filename().u8string() == "Contents" && res.has_parent_path())
    {
      // We are properly situated in a bundle in either a MacOS ir an iOS location
      return res.parent_path();
    }
  }
  catch (const fs::filesystem_error &)
  {
    // oh well
  }
  return {};
}

std::vector<fs::path> getMacCLAPSearchPaths()
{
  auto res = std::vector<fs::path>();

  auto bundlePath = sharedLibraryBundlePath();
  if (!bundlePath.empty())
  {
    std::string name = bundlePath.u8string();
    CFURLRef bundleUrl = CFURLCreateFromFileSystemRepresentation(
        nullptr, (const unsigned char *)name.c_str(), name.size(), true);
    if (bundleUrl)
    {
      auto pluginBundle = CFBundleCreate(nullptr, bundleUrl);
      CFRelease(bundleUrl);

      if (pluginBundle)
      {
        auto pluginFoldersUrl = CFBundleCopyBuiltInPlugInsURL(pluginBundle);

        if (pluginFoldersUrl)
        {
          // Remember CFURL and NSURL are toll free bridged
          auto *ns = (NSURL *)pluginFoldersUrl;
          auto pp = fs::path{[ns fileSystemRepresentation]};
          res.push_back(pp);
          CFRelease(pluginFoldersUrl);
        }
        CFRelease(pluginBundle);
      }
    }
  }

  auto *fileManager = [NSFileManager defaultManager];
  auto *userLibURLs = [fileManager URLsForDirectory:NSLibraryDirectory inDomains:NSUserDomainMask];
  auto *sysLibURLs = [fileManager URLsForDirectory:NSLibraryDirectory inDomains:NSLocalDomainMask];

  if (userLibURLs)
  {
    auto *u = [userLibURLs objectAtIndex:0];
    auto p = fs::path{[u fileSystemRepresentation]} / "Audio" / "Plug-Ins" / "CLAP";
    res.push_back(p);
  }

  if (sysLibURLs)
  {
    auto *u = [sysLibURLs objectAtIndex:0];
    auto p = fs::path{[u fileSystemRepresentation]} / "Audio" / "Plug-Ins" / "CLAP";
    res.push_back(p);
  }
  return res;
}
std::string toString(const CFStringRef aString ) {
  if (aString == NULL) {
    return std::string();
  }

  std::string result;

  CFIndex length = CFStringGetLength(aString);
  CFIndex maxSize =
  CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  result.reserve(maxSize);
  
  if (CFStringGetCString(aString, result.data(), maxSize,
                         kCFStringEncodingUTF8)) {
    return result;
  }
  
  return std::string();
}
}  // namespace Clap
