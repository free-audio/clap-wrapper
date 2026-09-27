
//
//  wrapped view for clap-wrapper
//
//  created by Paul Walker (baconpaul) and Timo Kaluza (defiantnerd)
//
//  this file needs some macros to be defined before being included
//  the wrapper's build-helper creates an intermediate file for this.
//  for more information, take a look into wrappedview.mm

#include <objc/runtime.h>
#import <CoreFoundation/CoreFoundation.h>
#import <AudioUnit/AUCocoaUIView.h>
#import <AppKit/AppKit.h>

#include "auv2_shared.h"
#include <detail/os/osutil.h>

//#define CLAP_WRAPPER_UI_CLASSNAME_NSVIEW CLAP_WRAPPER_COCOA_CLASS_NSVIEW
//#define CLAP_WRAPPER_UI_CLASSNAME_COCOAUI CLAP_WRAPPER_COCOA_CLASS

@interface CLAP_WRAPPER_COCOA_CLASS_NSVIEW : NSView <ClapWrapperAUv2ResizableView>
{
  free_audio::auv2_wrapper::ui_connection ui;
  uint32_t canary;
  CFRunLoopTimerRef idleTimer;
  // the size the plugin was last told to be, so a host laying the view out
  // repeatedly at the same size does not keep calling back into it
  NSSize underlyingUISize;
  bool setSizeByZoom;  // use this flag to see if resize comes from here or from external
}

- (id)initWithAUv2:(free_audio::auv2_wrapper::ui_connection *)cont preferredSize:(NSSize)size;
- (void)doIdle;
- (void)dealloc;
- (void)setFrame:(NSRect)newSize;
- (void)setFrameSize:(NSSize)newSize;

@end

@interface CLAP_WRAPPER_COCOA_CLASS : NSObject <AUCocoaUIBase>
{
}
- (NSString *)description;
@end

@implementation CLAP_WRAPPER_COCOA_CLASS

- (NSView *)uiViewForAudioUnit:(AudioUnit)inAudioUnit withSize:(NSSize)inPreferredSize
{
  free_audio::auv2_wrapper::ui_connection uiconn;

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
}

- (unsigned int)interfaceVersion
{
  LOGINFO("[clap-wrapper] get interface version");
  return 0;
}

- (NSString *)description
{
  LOGINFO("[clap-wrapper] get description: " CLAP_WRAPPER_EDITOR_NAME);
  return [NSString stringWithUTF8String:CLAP_WRAPPER_EDITOR_NAME];
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
  // The property lookup has returned, so editor calls need their own SDK entry guard.
  const auto mainThreadMutex = ui._mainThreadMutex;
  const ausdk::AUEntryGuard mainThreadGuard(mainThreadMutex.get());
  canary = 0xbeebbeeb;

  if (ui._registerWindow)
  {
    ui._registerWindow((clap_window_t *)self, &canary);
  }
  ui._createWindow();
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
  // Nothing AppKit does while the view is being built is a resize the host
  // asked for, and the size below is the one the plugin just handed us through
  // get_size(). The explicit set_size() further down is what tells it the size
  // it ends up at.
  setSizeByZoom = true;
  self = [super initWithFrame:NSMakeRect(0, 0, size.width, size.height)];
  setSizeByZoom = false;

  // gui->show(ui._plugin->_plugin);

  clap_window_t m;
  m.api = CLAP_WINDOW_API_COCOA;
  m.ptr = self;

  gui->set_parent(ui._plugin->_plugin, &m);
  // gui->set_scale is intentionally not called because
  // AUv2 gui always uses logical size

  if (gui->can_resize(ui._plugin->_plugin))
  {
    clap_gui_resize_hints_t resize_hints;
    gui->get_resize_hints(ui._plugin->_plugin, &resize_hints);
    NSAutoresizingMaskOptions mask = 0;

    if (resize_hints.can_resize_horizontally) mask |= NSViewWidthSizable;
    if (resize_hints.can_resize_vertically) mask |= NSViewHeightSizable;

    [self setAutoresizingMask:mask];
    gui->set_size(ui._plugin->_plugin, size.width, size.height);
    underlyingUISize = size;
  }

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
- (void)viewDidMoveToWindow
{
  const auto mainThreadMutex = ui._mainThreadMutex;
  const ausdk::AUEntryGuard mainThreadGuard(mainThreadMutex.get());
  if ([self window] == nil)
  {
    LOGINFO("[clap-wrapper] - view removed from a window");
    if (idleTimer)
    {
      CFRunLoopTimerInvalidate(idleTimer);
      idleTimer = 0;
    }
    if (canary)
    {
      ui._destroyWindow();

      assert(canary == 0);
    }
  }
  [super viewDidMoveToWindow];
}

- (void)dealloc
{
  // Super deallocation releases the C++ ivars before this scope unlocks the mutex.
  const auto mainThreadMutex = ui._mainThreadMutex;
  const ausdk::AUEntryGuard mainThreadGuard(mainThreadMutex.get());
  LOGINFO("[clap-wrapper] NS View dealloc");
  if (idleTimer)
  {
    CFRunLoopTimerInvalidate(idleTimer);
  }
  if (canary)
  {
    LOGINFO("[clap-wrapper] the host did not call viewDidMoveWindow with a nil window");
    ui._destroyWindow();
  }
  [super dealloc];
}
// Whether a size change arriving from AppKit is one the plugin has a say in.
// setSizeByZoom means it is a size the plugin picked itself, on its way out
// through request_resize - it already knows it, and handing it back would make
// a plugin that recomputes its layout in set_size() and asks again for what it
// settles on bounce between the two sizes.
- (bool)clapWrapperShouldTellPluginAboutSize
{
  return canary && !setSizeByZoom && ui._plugin->_ext._gui->can_resize(ui._plugin->_plugin);
}

// What the plugin will actually take of the size a host wants. CLAP wants
// set_size() to carry a size the plugin agreed to, and the size a host lays the
// view out at is nothing of the sort - a fixed aspect ratio or a step size snaps
// it here. The VST3 view does the same in onSize().
- (NSSize)clapWrapperAdjustSize:(NSSize)size
{
  if (![self clapWrapperShouldTellPluginAboutSize]) return size;

  uint32_t w = (uint32_t)size.width;
  uint32_t h = (uint32_t)size.height;
  if (ui._plugin->_ext._gui->adjust_size(ui._plugin->_plugin, &w, &h)) return NSMakeSize(w, h);
  return size;
}

// Tells the plugin the size it is now at. Both -setFrame: and -setFrameSize:
// end here: AppKit funnels the former through the latter, but autoresizing
// calls -setFrameSize: on its own and neither is contractually the other's only
// route, so both have to ask. underlyingUISize collapses a pair that does
// arrive together, and keeps a host laying the view out repeatedly at one size
// from calling into the plugin over and over.
- (void)clapWrapperTellPluginAboutSize:(NSSize)size
{
  if (![self clapWrapperShouldTellPluginAboutSize]) return;

  const NSSize agreed = NSMakeSize((uint32_t)size.width, (uint32_t)size.height);
  if (NSEqualSizes(agreed, underlyingUISize)) return;

  // gui->set_scale is intentionally not called because
  // AUv2 gui always uses logical size
  ui._plugin->_ext._gui->set_size(ui._plugin->_plugin, (uint32_t)agreed.width, (uint32_t)agreed.height);
  underlyingUISize = agreed;
}

- (void)setFrame:(NSRect)newSize
{
  const auto mainThreadMutex = ui._mainThreadMutex;
  const ausdk::AUEntryGuard mainThreadGuard(mainThreadMutex.get());

  newSize.size = [self clapWrapperAdjustSize:newSize.size];
  [super setFrame:newSize];
  [self clapWrapperTellPluginAboutSize:newSize.size];
  // gui->show(ui._plugin->_plugin);
}

// The override -setFrame: alone does not cover: a superview resize drives
// autoresizing straight through here, so a host that lets its own window layout
// resize the view never went past the wrapper at all.
- (void)setFrameSize:(NSSize)newSize
{
  const auto mainThreadMutex = ui._mainThreadMutex;
  const ausdk::AUEntryGuard mainThreadGuard(mainThreadMutex.get());

  newSize = [self clapWrapperAdjustSize:newSize];
  [super setFrameSize:newSize];
  [self clapWrapperTellPluginAboutSize:newSize];
}

- (BOOL)clapWrapperRequestResizeToWidth:(uint32_t)width height:(uint32_t)height
{
  const auto mainThreadMutex = ui._mainThreadMutex;
  const ausdk::AUEntryGuard mainThreadGuard(mainThreadMutex.get());
  if (!canary) return NO;

  // Telling AppKit is the whole job here: the plugin picked these numbers, so
  // -setFrame: must not hand them straight back to it. The host sees the frame
  // change and comes back through -setFrame: with what it could actually give
  // us, which is the size the plugin then hears about.
  NSRect frame = [self frame];
  frame.size = NSMakeSize(width, height);

  setSizeByZoom = true;
  [self setFrame:frame];
  setSizeByZoom = false;

  underlyingUISize = frame.size;
  return YES;
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
