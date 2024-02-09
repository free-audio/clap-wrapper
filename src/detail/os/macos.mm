#define NOMINMAX 1

/*
    the macos helper
    
    provides services for all plugin instances regarding macos
     - global timer object
     - dispatch to UI thread
     - get the bundle name
 
*/

#include <Foundation/Foundation.h>
#ifdef CLAP_WRAPPER_BUILD_FOR_VST3
#include "public.sdk/source/main/moduleinit.h"
#endif
#include "osutil.h"
#include <vector>
#if MACOS_USE_STD_FILESYSTEM
#include <filesystem>
namespace fs = std::filesystem;
#else
#include "ghc/filesystem.hpp"
namespace fs = ghc::filesystem;
#endif
#include <iostream>
#include <dlfcn.h>

namespace os
{
void log(const char* text)
{
  printf("%s\n", text);
  // NSLog(@"%s", text);
}

class MacOSHelper
{
 public:
  void init();
  void terminate();
  void attach(IPlugObject* plugobject);
  void detach(IPlugObject* plugobject);

 private:
  static void timerCallback(CFRunLoopTimerRef t, void* info);
  void executeDefered();
  CFRunLoopTimerRef _timer = nullptr;
  std::vector<IPlugObject*> _plugs;
} gMacOSHelper;

// standard specific extensions
// ----------------------------------------------------------
#ifdef CLAP_WRAPPER_BUILD_FOR_VST3
static Steinberg::ModuleInitializer createMessageWindow([] { gMacOSHelper.init(); });
static Steinberg::ModuleTerminator dropMessageWindow([] { gMacOSHelper.terminate(); });
#endif
// ----------------------------------------------------------

void MacOSHelper::init()
{
}

void MacOSHelper::terminate()
{
}

void MacOSHelper::executeDefered()
{
  for (auto p : _plugs)
  {
    if (p) p->onIdle();
  }
}

void MacOSHelper::timerCallback(CFRunLoopTimerRef /*t*/, void* info)
{
  auto self = static_cast<MacOSHelper*>(info);
  self->executeDefered();
}

static float kIntervall = 10.f;

void MacOSHelper::attach(IPlugObject* plugobject)
{
  if (_plugs.empty())
  {
    CFRunLoopTimerContext context = {};
    context.info = this;
    _timer =
        CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + (kIntervall * 0.001f),
                             kIntervall * 0.001f, 0, 0, timerCallback, &context);
    if (_timer) CFRunLoopAddTimer(CFRunLoopGetCurrent(), _timer, kCFRunLoopCommonModes);
  }
  _plugs.push_back(plugobject);
}

void MacOSHelper::detach(IPlugObject* plugobject)
{
  _plugs.erase(std::remove(_plugs.begin(), _plugs.end(), plugobject), _plugs.end());
  if (_plugs.empty())
  {
    if (_timer)
    {
      CFRunLoopTimerInvalidate(_timer);
      CFRelease(_timer);
    }
    _timer = nullptr;
  }
}

}  // namespace os

namespace os
{
// [UI Thread]
void attach(IPlugObject* plugobject)
{
  gMacOSHelper.attach(plugobject);
}

// [UI Thread]
void detach(IPlugObject* plugobject)
{
  gMacOSHelper.detach(plugobject);
}

uint64_t getTickInMS()
{
  return (::clock() * 1000) / CLOCKS_PER_SEC;
}

fs::path getBundlePath()
{
  Dl_info info;
  if (dladdr((void*)getBundlePath, &info))
  {
    fs::path binaryPath = info.dli_fname;
    return binaryPath.parent_path().parent_path().parent_path();
  }

  return {};
}

std::string getParentFolderName()
{
  return getBundlePath().parent_path().stem();
}

std::string getBinaryName()
{
  return getBundlePath().stem();
}

}  // namespace os
