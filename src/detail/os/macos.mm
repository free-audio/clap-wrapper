#define NOMINMAX 1

/**
*		the macos helper
* 
*		provides services for all plugin instances regarding macos
*        - global timer object
*        - dispatch to UI thread
*        - get the bundle name
* 
*/

#include <Foundation/Foundation.h>
#include "public.sdk/source/main/moduleinit.h"
#include "osutil.h"
#include <vector>
#include <filesystem>
#include <iostream>

namespace os
{
	void log(const char* text)
	{
	  NSLog(@"%s", text);
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

	static Steinberg::ModuleInitializer createMessageWindow([] { gMacOSHelper.init(); });
	static Steinberg::ModuleTerminator dropMessageWindow([] { gMacOSHelper.terminate(); });

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
			p->onIdle();
		}
	}

  void MacOSHelper::timerCallback(CFRunLoopTimerRef t, void* info)
  {
    auto self = static_cast<MacOSHelper*>(info);
    self->executeDefered();
  }

  static float kIntervall = 10.f;

	void MacOSHelper::attach(IPlugObject* plugobject)
	{
	  if ( _plugs.empty() )
	  {
	  	CFRunLoopTimerContext context = {};
	  	context.info = this;
	  	_timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + (kIntervall * 0.001f), kIntervall * 0.001f, 0, 0, timerCallback, &context);
	  	CFRunLoopAddTimer(CFRunLoopGetCurrent(), _timer, kCFRunLoopCommonModes);
	  }
	  _plugs.push_back(plugobject);
	}

	void MacOSHelper::detach(IPlugObject * plugobject)
	{
	  _plugs.erase(std::remove(_plugs.begin(), _plugs.end(), plugobject), _plugs.end());
	  if ( _plugs.empty() )
	  {
	    CFRunLoopTimerInvalidate(_timer);
	    CFRelease(_timer);
	    _timer = nullptr;
	  }
	}

}

// the dummy class so we can use NSBundle bundleForClass
@interface clapwrapper_dummy_object_to_trick_the_os : NSObject
- (void) fun;
@end

@implementation clapwrapper_dummy_object_to_trick_the_os
- (void) fun{}
@end

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

  std::string getParentFolderName()
  {
    NSString* identifier = [[NSBundle bundleForClass:[clapwrapper_dummy_object_to_trick_the_os class]] bundlePath];
    std::filesystem::path n = [identifier UTF8String];
    if (n.has_parent_path())
    {
      auto p = n.parent_path();
      if (p.has_filename())
      {
        return p.filename().u8string();
      }
    }
    
    return std::string();
  }

  std::string getBinaryName()
  {
    // this is useless
    // NSString* identifier = [[NSBundle mainBundle] bundleIdentifier];
    
    // this is needed:
    NSString* identifier = [[NSBundle bundleForClass:[clapwrapper_dummy_object_to_trick_the_os class]] bundlePath];
    std::filesystem::path k = [identifier UTF8String];
    return k.stem();
  }
}
