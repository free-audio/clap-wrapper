
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
  ui._plugin->_ext._gui->create(ui._plugin->_plugin, CLAP_WINDOW_API_COCOA, false);
  auto gui = ui._plugin->_ext._gui;

  // actually, the host should send an appropriate size,
  // yet, they actually just send utter garbage, so: don't care
  // if (size.width == 0 || size.height == 0)
  {
    // gui->get_size(ui._plugin->_plugin,)
    uint32_t w, h;
    if (gui->get_size(ui._plugin->_plugin, &w, &h))
    {
      size = {(double)w, (double)h};
    }
  }
  self = [super initWithFrame:NSMakeRect(0, 0, size.width, size.height)];

  // gui->show(ui._plugin->_plugin);

  clap_window_t m;
  m.api = CLAP_WINDOW_API_COCOA;
  m.ptr = self;

  gui->set_parent(ui._plugin->_plugin, &m);
  gui->set_scale(ui._plugin->_plugin, 1.0);

  if (gui->can_resize(ui._plugin->_plugin)) gui->set_size(ui._plugin->_plugin, size.width, size.height);

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
  // auto gui = ui._plugin->_ext._gui;
}
- (void)dealloc
{
  LOGINFO("[clap-wrapper] NS View dealloc");
  if (idleTimer)
  {
    CFRunLoopTimerInvalidate(idleTimer);
  }
  auto gui = ui._plugin->_ext._gui;
  gui->destroy(ui._plugin->_plugin);

  [super dealloc];
}
- (void)setFrame:(NSRect)newSize
{
  LOGINFO("[clap-wrapper] new size");

  [super setFrame:newSize];
  auto gui = ui._plugin->_ext._gui;
  gui->set_scale(ui._plugin->_plugin, 1.0);
  gui->set_size(ui._plugin->_plugin, newSize.size.width, newSize.size.height);

  // gui->show(ui._plugin->_plugin);
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
