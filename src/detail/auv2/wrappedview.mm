
//
//  wrapped view for clap-wrapper
//
//  created by Paul Walker (baconpaul) and Timo Kaluza (defiantnerd)
//


#include <objc/runtime.h>
#import <CoreFoundation/CoreFoundation.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <AppKit/AppKit.h>

#include "wrappedview.h"
#include "auv2_shared.h"
#include <detail/os/osutil.h>

//#define CLAP_WRAPPER_UI_CLASSNAME_NSVIEW Clap_Wrapper_View_NSView
//#define CLAP_WRAPPER_UI_CLASSNAME_COCOAUI Clap_Wrapper_View_CocoaUI

@interface Clap_Wrapper_View_NSView : NSView
{
    free_audio::auv2_wrapper::ui_connection ui;
    CFRunLoopTimerRef idleTimer;
    float lastScale;
    NSSize underlyingUISize;
    bool setSizeByZoom; // use this flag to see if resize comes from here or from external
}

- (id)initWithAUv2:(free_audio::auv2_wrapper::ui_connection *)cont preferredSize:(NSSize)size;
- (void)doIdle;
- (void)dealloc;
- (void)setFrame:(NSRect)newSize;

@end

@interface Clap_Wrapper_View_CocoaUI : NSObject <AUCocoaUIBase>
{
}
- (NSString *)description;
@end

@implementation Clap_Wrapper_View_CocoaUI

free_audio::auv2_wrapper::ui_connection uiconn;

- (NSView *)uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize
{
  // free_audio::auv2_wrapper::ui_connection connection;
  // Remember we end up being called here because that's what AUCocoaUIView does in the initiation
  // collaboration with hosts
  
  UInt32 size = sizeof(free_audio::auv2_wrapper::ui_connection);
  if (AudioUnitGetProperty(inAudioUnit, kAudioUnitProperty_ClapWrapper_UIConnection_id,
                           kAudioUnitScope_Global, 0, &uiconn, &size) != noErr)
    return nil;
  
  return [[[Clap_Wrapper_View_NSView alloc] initWithAUv2:&uiconn
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

void timerCallback(CFRunLoopTimerRef timer, void *info)
{
  Clap_Wrapper_View_NSView *view = (Clap_Wrapper_View_NSView *)info;
    [view doIdle];
}

@implementation Clap_Wrapper_View_NSView

- (id)initWithAUv2:(free_audio::auv2_wrapper::ui_connection *)cont preferredSize:(NSSize)size
{
  LOGINFO("[clap-wrapper] create NS View begin");
  
  ui = *cont;
  ui._plugin->_ext._gui->create(ui._plugin->_plugin, CLAP_WINDOW_API_COCOA, false);
  auto gui = ui._plugin->_ext._gui;
  
  // actually, the host should send an appropriate size,
  // yet, they actually just send utter garbage, so: don't care
  // if (size.width == 0 || size.height == 0)
  {
    // gui->get_size(ui._plugin->_plugin,)
    uint32_t w,h;
    if ( gui->get_size(ui._plugin->_plugin,&w,&h) )
    {
      size = { (double)w,(double)h };
    }
  }
  self = [super initWithFrame:NSMakeRect(0, 0, size.width , size.height)];
  
  
  // gui->show(ui._plugin->_plugin);

  clap_window_t m;
  m.api = CLAP_WINDOW_API_COCOA;
  m.ptr = self;
  
  gui->set_parent(ui._plugin->_plugin, &m);
  gui->set_scale(ui._plugin->_plugin, 1.0);
  gui->set_size(ui._plugin->_plugin, size.width, size.height);
  
  idleTimer = nil;
  CFTimeInterval TIMER_INTERVAL = .05; // In SurgeGUISynthesizer.h it uses 50 ms
  CFRunLoopTimerContext TimerContext = {0, self, NULL, NULL, NULL};
  CFAbsoluteTime FireTime = CFAbsoluteTimeGetCurrent() + TIMER_INTERVAL;
  idleTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, FireTime, TIMER_INTERVAL, 0, 0,
                                   timerCallback, &TimerContext);
  if (idleTimer)
      CFRunLoopAddTimer(CFRunLoopGetMain(), idleTimer, kCFRunLoopCommonModes);
  
  LOGINFO("[clap-wrapper] create NS View end");
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
  gui->set_size(ui._plugin->_plugin,newSize.size.width,newSize.size.height);
  
  
  // gui->show(ui._plugin->_plugin);

}

@end

bool fillAudioUnitCocoaView(AudioUnitCocoaViewInfo* viewInfo)
{
  // now we are in m&m land.. 
  auto bundle = [NSBundle bundleForClass:[Clap_Wrapper_View_CocoaUI class]];
  
  LOGINFO("[clap-wrapper] fill AudioUnitCocoaViewInfo");
  if ( bundle )
  {
    // Get the URL for the main bundle
    NSURL *url = [bundle bundleURL];
    CFURLRef cfUrl = (__bridge CFURLRef)url;
    CFRetain(cfUrl);
    
    CFStringRef className = CFSTR("Clap_Wrapper_View_CocoaUI");
    
    *viewInfo = { cfUrl, { className }};
    LOGINFO("[clap-wrapper] fill AudioUnitCocoaViewInfo: OK");
    return true;
    
  }
  LOGINFO("[clap-wrapper] fill AudioUnitCocoaViewInfo: FAILED");
  return false;
}

