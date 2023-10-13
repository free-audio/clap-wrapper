
//
//  wrapped view for clap-wrapper
//
//  created by Paul Walker (baconpaul) and Timo Kaluza (defiantnerd)
//

#include <objc/runtime.h>
#import <CoreFoundation/CoreFoundation.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <AppKit/AppKit.h>

#include "auv2_shared.h"
#include <detail/os/osutil.h>

//#define CLAP_WRAPPER_UI_CLASSNAME_NSVIEW CLAP_WRAPPER_COCOA_CLASS_NSVIEW
//#define CLAP_WRAPPER_UI_CLASSNAME_COCOAUI CLAP_WRAPPER_COCOA_CLASS

@interface CLAP_WRAPPER_COCOA_CLASS_NSVIEW : NSView
{
  free_audio::auv2_wrapper::ui_connection ui;
  CFRunLoopTimerRef idleTimer;
  float lastScale;
  NSSize underlyingUISize;
  bool setSizeByZoom;  // use this flag to see if resize comes from here or from external
}

- (id)initWithAUv2:(free_audio::auv2_wrapper::ui_connection *)cont preferredSize:(NSSize)size;
- (void)doIdle;
- (void)dealloc;
- (void)setFrame:(NSRect)newSize;

@end

@interface CLAP_WRAPPER_COCOA_CLASS : NSObject <AUCocoaUIBase>
{
}
- (NSString *)description;
@end

@implementation CLAP_WRAPPER_COCOA_CLASS

- (NSView *)uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize
{
  static free_audio::auv2_wrapper::ui_connection uiconn;

  // free_audio::auv2_wrapper::ui_connection connection;
  // Remember we end up being called here because that's what AUCocoaUIView does in the initiation
  // collaboration with hosts

  UInt32 size = sizeof(free_audio::auv2_wrapper::ui_connection);
  if (AudioUnitGetProperty(inAudioUnit, kAudioUnitProperty_ClapWrapper_UIConnection_id,
                           kAudioUnitScope_Global, 0, &uiconn, &size) != noErr)
    return nil;

  return [[[CLAP_WRAPPER_COCOA_CLASS_NSVIEW alloc] initWithAUv2:&uiconn
                                                  preferredSize:inPreferredSize] autorelease];
  LOGINFO("[clap-wrapper] get ui View for AudioUnit");
  // return nil;
}

- (unsigned int)interfaceVersion
{
  LOGINFO("[clap-wrapper] get interface version");
  return 0;
}

- (NSString *)description
{
  LOGINFO("[clap-wrapper] get description");
  return [NSString stringWithUTF8String:"Wrap Window"];  // TODO: get name from plugin
}

@end

void CLAP_WRAPPER_TIMER_CALLBACK(CFRunLoopTimerRef timer, void *info)
{
  CLAP_WRAPPER_COCOA_CLASS_NSVIEW *view = (CLAP_WRAPPER_COCOA_CLASS_NSVIEW *)info;
  [view doIdle];
}

@implementation CLAP_WRAPPER_COCOA_CLASS_NSVIEW

- (id)initWithAUv2:(free_audio::auv2_wrapper::ui_connection *)cont preferredSize:(NSSize)size
{
  LOGINFO("[clap-wrapper] creating NSView");

  ui = *cont;
  auto pluginProxy = ui._plugin->getProxy();
  pluginProxy->guiCreate(CLAP_WINDOW_API_COCOA, false);

  // actually, the host should send an appropriate size,
  // yet, they actually just send utter garbage, so: don't care
  // if (size.width == 0 || size.height == 0)
  {
    // pluginProxy->guiGetSize(,)
    uint32_t w, h;
    if (pluginProxy->guiGetSize(&w, &h))
    {
      size = {(double)w, (double)h};
    }
  }
  self = [super initWithFrame:NSMakeRect(0, 0, size.width, size.height)];

  // pluginProxy->guiShow();

  clap_window_t m;
  m.api = CLAP_WINDOW_API_COCOA;
  m.ptr = self;

  pluginProxy->guiSetParent(&m);
  pluginProxy->guiSetScale(1.0);

  if (pluginProxy->guiCanResize()) pluginProxy->guiSetSize(size.width, size.height);

  idleTimer = nil;
  CFTimeInterval TIMER_INTERVAL = .05;  // In SurgeGUISynthesizer.h it uses 50 ms
  CFRunLoopTimerContext TimerContext = {0, self, NULL, NULL, NULL};
  CFAbsoluteTime FireTime = CFAbsoluteTimeGetCurrent() + TIMER_INTERVAL;
  idleTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, FireTime, TIMER_INTERVAL, 0, 0,
                                   CLAP_WRAPPER_TIMER_CALLBACK, &TimerContext);
  if (idleTimer) CFRunLoopAddTimer(CFRunLoopGetMain(), idleTimer, kCFRunLoopCommonModes);

  return self;
}

- (void)doIdle
{
}
- (void)dealloc
{
  LOGINFO("[clap-wrapper] NS View dealloc");
  if (idleTimer)
  {
    CFRunLoopTimerInvalidate(idleTimer);
  }
  auto pluginProxy = ui._plugin->getProxy();
  pluginProxy->guiDestroy();

  [super dealloc];
}
- (void)setFrame:(NSRect)newSize
{
  LOGINFO("[clap-wrapper] new size");

  [super setFrame:newSize];
  auto pluginProxy = ui._plugin->getProxy();
  pluginProxy->guiSetScale(1.0);
  pluginProxy->guiSetSize(newSize.size.width, newSize.size.height);

  // pluginProxy->guiShow();
}

@end

bool CLAP_WRAPPER_FILL_AUCV(AudioUnitCocoaViewInfo *viewInfo)
{
  // now we are in m&m land..
  auto bundle = [NSBundle bundleForClass:[CLAP_WRAPPER_COCOA_CLASS class]];

  if (bundle)
  {
    // Get the URL for the main bundle
    NSURL *url = [bundle bundleURL];
    CFURLRef cfUrl = (__bridge CFURLRef)url;
    CFRetain(cfUrl);

#define ascf(x) CFSTR(#x)
#define tocf(x) ascf(x)
    CFStringRef className = tocf(CLAP_WRAPPER_COCOA_CLASS);
#undef tocf
#undef ascf

    *viewInfo = {cfUrl, {className}};
    LOGINFO("[clap-wrapper] created AudioUnitCocoaView: - class is \"{}\"",
            CFStringGetCStringPtr(className, kCFStringEncodingUTF8));
    return true;
  }
  LOGINFO("[clap-wrapper] create AudioUnitCocoaView failed: {}", __func__);
  return false;
}
