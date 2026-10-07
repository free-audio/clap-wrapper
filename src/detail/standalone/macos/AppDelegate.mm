#import "AppDelegate.h"
#import "StandardMenuBar.h"

#include <AVFoundation/AVFoundation.h>

#include <map>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"

#include "detail/clap/fsutil.h"

#if !__has_feature(objc_arc)
#error "the macOS standalone sources are built with -fobjc-arc"
#endif

@interface AudioSettingsWindow : NSWindow
{
  NSPopUpButton *outputSelection, *inputSelection, *sampleRateSelection;
  std::vector<RtAudio::DeviceInfo> outDevices, inDevices;
}

- (void)setupContents;
- (void)resetSampleRateSelection;

@end

@interface ClapWrapperAppDelegate ()
{
  AudioSettingsWindow *audioSettingsWindow;
  NSURL *currentFile;
}

@end

static void showError(NSString *message, NSString *info)
{
  NSAlert *alert = [[NSAlert alloc] init];
  [alert setMessageText:message];
  [alert setInformativeText:info];
  [alert addButtonWithTitle:@"OK"];
  [alert runModal];
}

@implementation ClapWrapperAppDelegate

- (void)timerCallback:(NSTimer *)instance
{
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  if (standaloneHost->callbackRequested.exchange(false))
  {
    auto *plugin = freeaudio::clap_wrapper::standalone::getMainPlugin()->_plugin;
    plugin->on_main_thread(plugin);
  }

  if (standaloneHost->restartRequested.exchange(false))
  {
    // manually set running to false to make clapProcess a no-op
    // while the plugin is being reactivated. otherwise,
    // stopping and starting the entire audio engine is probably
    // overkill.
    {
      ClapWrapper::detail::shared::SpinLockGuard g(standaloneHost->processLock);
      standaloneHost->running = false;
    }
    // stay stopped if the plugin refused to reactivate
    standaloneHost->running = standaloneHost->activatePlugin(standaloneHost->currentSampleRate, 1,
                                                             standaloneHost->currentBufferSize * 2);
  }

  // Unlike a plugin wrapper, the standalone owns its audio engine, so a wake
  // request means something here: if the plugin is not active, stand it up. If
  // it already is, the callback is pulling it and there is nothing to do. The
  // sample rate check keeps this from firing before the engine has ever run.
  if (standaloneHost->processRequested.exchange(false) && !standaloneHost->isActive &&
      standaloneHost->currentSampleRate > 0)
  {
    standaloneHost->running = standaloneHost->activatePlugin(standaloneHost->currentSampleRate, 1,
                                                             standaloneHost->currentBufferSize * 2);
  }
}

- (void)createWindowWithContentSize:(NSSize)size resizable:(BOOL)resizable
{
  auto style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
  if (resizable) style |= NSWindowStyleMaskResizable;

  auto *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, size.width, size.height)
                                             styleMask:style
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
  // we own it; AppKit releasing it on close left the timer and gui talking to a freed window
  window.releasedWhenClosed = NO;
  window.title = [NSString stringWithUTF8String:OUTPUT_NAME];
  window.delegate = self;
  [window center];

  // restores the saved frame, then we put the size back to what the plugin asked for
  [window setFrameAutosaveName:@"ClapWrapperStandaloneMainWindow"];
  auto frame = window.frame;
  auto top = NSMaxY(frame);
  frame.size = [window frameRectForContentRect:NSMakeRect(0, 0, size.width, size.height)].size;
  frame.origin.y = top - frame.size.height;
  [window setFrame:frame display:NO];

  self.window = window;
}

- (void)showWindow
{
  [self.window makeKeyAndOrderFront:nil];
#if defined(MAC_OS_VERSION_14_0)
  if (@available(macOS 14.0, *))
  {
    [NSApp activate];
    return;
  }
#endif
  [NSApp activateIgnoringOtherApps:YES];
}

- (void)showEmptyWindow
{
  [self createWindowWithContentSize:NSMakeSize(480, 360) resizable:NO];
  [self showWindow];
}

- (void)doSetup
{
  const char *argv[2] = {OUTPUT_NAME, 0};

  const clap_plugin_entry *entry{nullptr};
#ifdef STATICALLY_LINKED_CLAP_ENTRY
  extern const clap_plugin_entry clap_entry;
  entry = &clap_entry;
#else
  // Library shenanigans t/k
  std::string clapName{HOSTED_CLAP_NAME};
  LOGINFO("Loading '{}'", clapName);

  auto pts = Clap::getValidCLAPSearchPaths();

  auto lib = Clap::Library();

  for (const auto &searchPaths : pts)
  {
    auto clapPath = searchPaths / (clapName + ".clap");

    if (fs::is_directory(clapPath) && !entry)
    {
      lib.load(clapPath);
      entry = lib._pluginEntry;
    }
  }
#endif

  if (!entry)
  {
    [self showEmptyWindow];
    return;
  }
  self.requestCallbackTimer = [NSTimer timerWithTimeInterval:0.08
                                                      target:self
                                                    selector:@selector(timerCallback:)
                                                    userInfo:nil
                                                     repeats:YES];
  auto *runLoop = [NSRunLoop currentRunLoop];
  [runLoop addTimer:self.requestCallbackTimer forMode:NSRunLoopCommonModes];
  std::string pid{PLUGIN_ID};
  int pindex{PLUGIN_INDEX};

#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 101400
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
  {
    case AVAuthorizationStatusNotDetermined:
    {
      // The app hasn't yet asked the user for camera access.
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                               completionHandler:^(BOOL granted) {
                                 if (granted)
                                 {
                                 }
                               }];
      break;
    }
    default:
      break;
  }
#endif

  auto plugin =
      freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, (char **)argv);

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize =
      [self](uint32_t w, uint32_t h)
  {
    [self.window setContentSize:NSMakeSize(w, h)];
    // The size was accepted -- it has just been applied. Returning false said the
    // opposite, and a plugin that believed it left its own GUI a size behind the
    // window it is drawn in. Windows answers true from the same place.
    return true;
  };

  if (plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto p = plugin->_plugin;
    if (!ui->is_api_supported(p, CLAP_WINDOW_API_COCOA, false))
      LOGINFO("[WARNING] GUI API not supported");

    if (!ui->create(p, CLAP_WINDOW_API_COCOA, false))
    {
      LOGINFO("[ERROR] Plugin GUI create() failed");
      [self showEmptyWindow];
      showError(@"Plugin Error", @"The plugin failed to create its user interface.");
      return;
    }
    ui->set_scale(p, 1);

    uint32_t w = 0, h = 0;
    bool sizeValid = ui->get_size(p, &w, &h);
    NSString *sizeError = nil;
    if (!sizeValid)
      sizeError = @"The plugin failed to report its window size.";
    else if (w == 0 || h == 0 || w > 16384 || h > 16384)
      sizeError =
          [NSString stringWithFormat:@"The plugin reported an invalid window size (%u x %u).", w, h];

    if (sizeError)
    {
      LOGINFO("[ERROR] Plugin GUI get_size() failed: {}", [sizeError UTF8String]);
      ui->destroy(p);
      [self showEmptyWindow];
      showError(@"Plugin Error", sizeError);
      return;
    }

    auto canResize = ui->can_resize(p);
    if (canResize)
    {
      ui->adjust_size(p, &w, &h);
    }

    [self createWindowWithContentSize:NSMakeSize(w, h) resizable:canResize];

    clap_window win;
    win.api = CLAP_WINDOW_API_COCOA;
    win.cocoa = (__bridge void *)self.window.contentView;
    if (!ui->set_parent(p, &win))
    {
      LOGINFO("[ERROR] Plugin GUI set_parent() failed");
      [self showWindow];
      showError(@"Plugin Error",
                @"The plugin failed to embed its user interface. Please contact the plugin "
                @"developer.");
      return;
    }
    ui->show(p);
    // after the embed, so the window never flashes up blank
    [self showWindow];
  }
  else
  {
    [self showEmptyWindow];
  }

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = [](auto &s)
  {
    NSLog(@"Error Reported: %s", s.c_str());
    @autoreleasepool
    {
      showError(@"Unable to configure audio", [[NSString alloc] initWithUTF8String:s.c_str()]);
    }
  };

  freeaudio::clap_wrapper::standalone::mainStartAudio();
}

- (void)applicationWillFinishLaunching:(NSNotification *)aNotification
{
  auto *file = freeaudio::clap_wrapper::standalone::macos::installStandardMenuBar(
      @selector(openAudioSettingsWindow:), @"Audio/MIDI Settings…");

  auto *open = [[NSMenuItem alloc] initWithTitle:@"Open…"
                                          action:@selector(openWrapperFile:)
                                   keyEquivalent:@"o"];
  [file insertItem:open atIndex:0];
  [file insertItem:NSMenuItem.separatorItem atIndex:1];
  [file addItemWithTitle:@"Save" action:@selector(streamWrapperFile:) keyEquivalent:@"s"];
  auto *saveAs = [file addItemWithTitle:@"Save As…"
                                 action:@selector(streamWrapperFileAs:)
                          keyEquivalent:@"s"];
  saveAs.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
  [NSTimer scheduledTimerWithTimeInterval:0.001
                                   target:self
                                 selector:@selector(doSetup)
                                 userInfo:nil
                                  repeats:NO];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
  return true;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)app
{
  return YES;
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  LOGDETAIL("Application terminating");
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = nullptr;
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize = nullptr;

  // Scoped so this shared_ptr copy is released before mainFinish, which
  // deinit()s the entry: were it the last owner, ~Plugin would then call
  // _plugin->destroy() on a deinited - dynamically, unloaded - entry.
  {
    auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

    if (plugin && plugin->_ext._gui)
    {
      plugin->_ext._gui->hide(plugin->_plugin);
      plugin->_ext._gui->destroy(plugin->_plugin);
    }
  }

  // Before mainFinish: the callback dereferences getMainPlugin() unchecked.
  [self.requestCallbackTimer invalidate];
  self.requestCallbackTimer = nil;

  freeaudio::clap_wrapper::standalone::mainFinish();
}

- (IBAction)openAudioSettingsWindow:(id)sender
{
  if (audioSettingsWindow.visible)
  {
    [audioSettingsWindow makeKeyAndOrderFront:nil];
    return;
  }

  audioSettingsWindow = [[AudioSettingsWindow alloc]
      initWithContentRect:NSMakeRect(0, 0, 400, 360)
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  audioSettingsWindow.releasedWhenClosed = NO;

  [audioSettingsWindow setupContents];
  [audioSettingsWindow center];
  [audioSettingsWindow makeKeyAndOrderFront:nil];
}

- (void)windowWillClose:(NSNotification *)notification
{
  if (notification.object != self.window) return;

  // the plugin window is the app, so an open settings window must not keep it alive
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp terminate:nil];
  });
}

- (void)windowDidResize:(NSNotification *)notification
{
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  if (plugin && plugin->_ext._gui && plugin->_ext._gui->can_resize(plugin->_plugin))
  {
    auto cr = [self.window contentRectForFrameRect:self.window.frame];
    plugin->_ext._gui->set_size(plugin->_plugin, cr.size.width, cr.size.height);
  }
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize
{
  // only reachable when resizable, which means can_resize() said yes
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();
  if (!plugin || !plugin->_ext._gui) return frameSize;

  auto *gui = plugin->_ext._gui;
  auto current = [sender contentRectForFrameRect:sender.frame].size;
  auto f = sender.frame;
  f.size = frameSize;
  auto cr = [sender contentRectForFrameRect:f];

  clap_gui_resize_hints hints{};
  if (gui->get_resize_hints(plugin->_plugin, &hints))
  {
    if (!hints.can_resize_horizontally) cr.size.width = current.width;
    if (!hints.can_resize_vertically) cr.size.height = current.height;
  }

  uint32_t w = cr.size.width, h = cr.size.height;
  gui->adjust_size(plugin->_plugin, &w, &h);
  cr.size.width = w;
  cr.size.height = h;
  return [sender frameRectForContentRect:cr].size;
}

- (void)saveToURL:(NSURL *)url
{
  auto fsp = fs::path{[[url path] UTF8String]};
  auto fn = fsp.replace_extension(".cwstream");

  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();

  try
  {
    standaloneHost->saveStandaloneAndPluginSettings(fn.parent_path(), fn.filename());
    currentFile = [[url URLByDeletingPathExtension] URLByAppendingPathExtension:@"cwstream"];
  }
  catch (const fs::filesystem_error &e)
  {
    showError(@"Unable to save file", [[NSString alloc] initWithUTF8String:e.what()]);
  }
}

- (IBAction)streamWrapperFile:(id)sender
{
  if (currentFile)
    [self saveToURL:currentFile];
  else
    [self streamWrapperFileAs:sender];
}

- (IBAction)streamWrapperFileAs:(id)sender
{
  NSSavePanel *savePanel = [NSSavePanel savePanel];
  [savePanel setNameFieldStringValue:@"Untitled"];

  if ([savePanel runModal] == NSModalResponseOK)
  {
    [self saveToURL:[savePanel URL]];
  }
}

- (IBAction)openWrapperFile:(id)sender
{
  NSOpenPanel *openPanel = [NSOpenPanel openPanel];
  [openPanel setCanChooseFiles:YES];
  [openPanel setCanChooseDirectories:NO];
  [openPanel setAllowedFileTypes:[NSArray arrayWithObject:@"cwstream"]];
  [openPanel setAllowsMultipleSelection:NO];

  if ([openPanel runModal] == NSModalResponseOK)
  {
    NSURL *selectedUrl = [[openPanel URLs] objectAtIndex:0];

    auto fn = fs::path{[[selectedUrl path] UTF8String]};

    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();

    try
    {
      standaloneHost->tryLoadStandaloneAndPluginSettings(fn.parent_path(), fn.filename());
      currentFile = selectedUrl;
    }
    catch (const fs::filesystem_error &e)
    {
      showError(@"Unable to open file", [[NSString alloc] initWithUTF8String:e.what()]);
    }
  }
}

@end

@implementation AudioSettingsWindow

- (void)setupContents
{
  @autoreleasepool
  {
    auto addLabel = [](NSString *s, int x, int y)
    {
      NSTextField *label = [[NSTextField alloc] initWithFrame:NSMakeRect(x, y, 200, 30)];

      // Set the text of the label
      [label setStringValue:s];

      // By default, NSTextField objects are editable. Make this one non-editable and non-selectable to act like a label
      [label setEditable:NO];
      [label setSelectable:NO];
      [label setBezeled:NO];
      [label setDrawsBackground:NO];

      // Add the label to the window
      return label;
    };
    // Set the window title
    [self setTitle:@"Audio/MIDI Settings"];

    // Create the button
    NSButton *okButton = [[NSButton alloc] initWithFrame:NSMakeRect(400 - 80 - 80, 0, 80, 30)];
    [okButton setTitle:@"OK"];
    [okButton setTarget:self];
    [okButton setAction:@selector(okButtonPressed:)];

    [[self contentView] addSubview:okButton];

    NSButton *cancelButton = [[NSButton alloc] initWithFrame:NSMakeRect(400 - 80, 0, 80, 30)];
    [cancelButton setTitle:@"Cancel"];
    [cancelButton setTarget:self];
    [cancelButton setAction:@selector(cancelButtonPressed:)];

    [[self contentView] addSubview:addLabel(@"Output", 10, 320)];
    outputSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(100, 320, 290, 30)];

    [[self contentView] addSubview:addLabel(@"Sample Rate", 10, 285)];
    sampleRateSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(100, 285, 120, 30)];

    [[self contentView] addSubview:addLabel(@"Input", 10, 250)];
    inputSelection = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(100, 250, 290, 30)];

    NSButton *defaultButton = [[NSButton alloc] initWithFrame:NSMakeRect(95, 215, 300, 30)];
    [defaultButton setTitle:@"Reset to System Default"];
    [defaultButton setTarget:self];
    [defaultButton setAction:@selector(defaultButtonPressed:)];
    [[self contentView] addSubview:defaultButton];

    NSBox *horizontalRule = [[NSBox alloc] initWithFrame:NSMakeRect(10, 205, 380, 1)];
    [horizontalRule setBoxType:NSBoxSeparator];
    [[self contentView] addSubview:horizontalRule];

    [[self contentView] addSubview:addLabel(@"MIDI", 10, 165)];
    [[self contentView] addSubview:addLabel(@"Coming Soon", 100, 165)];

    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    outDevices = standaloneHost->getOutputAudioDevices();
    // add items to menu
    int selIdx{-1}, idx{0};
    //[outputSelection addItemWithTitle:@"No Output"];

    for (auto o : outDevices)
    {
      [outputSelection addItemWithTitle:[[NSString alloc] initWithUTF8String:o.name.c_str()]];
      if (standaloneHost->audioOutputDeviceID == o.ID) selIdx = idx;
      idx++;
    }
    if (selIdx >= 0)
    {
      [outputSelection selectItemAtIndex:selIdx];
    }
    [outputSelection setAction:@selector(onSourceMenuChanged:)];
    [outputSelection setTarget:self];

    inDevices = standaloneHost->getInputAudioDevices();
    selIdx = -1;
    idx = 1;
    [inputSelection addItemWithTitle:@"No Input"];

    for (auto i : inDevices)
    {
      [inputSelection addItemWithTitle:[[NSString alloc] initWithUTF8String:i.name.c_str()]];
      if (standaloneHost->audioInputDeviceID == i.ID) selIdx = idx;
      idx++;
    }
    if (selIdx >= 0)
    {
      [inputSelection selectItemAtIndex:selIdx];
    }

    [inputSelection setAction:@selector(onSourceMenuChanged:)];
    [inputSelection setTarget:self];

    [self resetSampleRateSelection];

    // Add the outputSelection to the window's content view
    [[self contentView] addSubview:outputSelection];
    [[self contentView] addSubview:inputSelection];
    [[self contentView] addSubview:sampleRateSelection];

    // Add the button to the window's content view
    [[self contentView] addSubview:cancelButton];
  }
}

- (void)okButtonPressed:(id)sender
{
  @autoreleasepool
  {
    unsigned int outId{0}, inId{0};
    bool useOut{false}, useIn{false};

    auto oidx = [outputSelection indexOfSelectedItem];
    if (oidx >= 0)  // modify this to > and add a -1 below if we add no out
    {
      const auto &oDev = outDevices[oidx];
      outId = oDev.ID;
      useOut = true;
    }

    auto iidx = [inputSelection indexOfSelectedItem];
    if (iidx > 0)
    {
      const auto &iDev = inDevices[iidx - 1];
      inId = iDev.ID;
      useIn = true;
    }

    const auto sr = [[[sampleRateSelection selectedItem] title] integerValue];

    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    standaloneHost->startAudioThreadOn(inId, 2, useIn, outId, 2, useOut, (int32_t)sr);

    [self close];
  }
}

- (void)defaultButtonPressed:(id)sender
{
  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  auto [in, out, sr] = standaloneHost->getDefaultAudioInOutSampleRate();
  int idx = 1;
  for (auto i : inDevices)
  {
    if ((int)i.ID == (int)in)
    {
      [inputSelection selectItemAtIndex:idx];
    }
    idx++;
  }

  idx = 0;
  for (auto o : outDevices)
  {
    if ((int)o.ID == (int)out)
    {
      [outputSelection selectItemAtIndex:idx];
    }
    idx++;
  }

  [self resetSampleRateSelection];

  for (NSMenuItem *item in [sampleRateSelection itemArray])
  {
    const auto sri = [[item title] integerValue];
    if ((int)sr == (int)sri)
    {
      [sampleRateSelection selectItem:item];
    }
  }
}

- (void)cancelButtonPressed:(id)sender
{
  @autoreleasepool
  {
    [self close];
  }
}

- (void)onSourceMenuChanged:(id)sender
{
  [self resetSampleRateSelection];
}

- (void)resetSampleRateSelection
{
  auto idx = [outputSelection indexOfSelectedItem];
  const auto &oDev = outDevices[idx];

  [sampleRateSelection removeAllItems];
  auto csr = freeaudio::clap_wrapper::standalone::getStandaloneHost()->currentSampleRate;

  std::map<int, int> srAvail;
  for (auto sr : oDev.sampleRates)
  {
    srAvail[sr]++;
  }

  idx = [inputSelection indexOfSelectedItem];
  if (idx > 0)
  {
    const auto &iDev = inDevices[idx - 1];

    for (auto sr : iDev.sampleRates)
    {
      srAvail[sr]++;
    }
  }
  else
  {
    // Just take the output rates
    for (auto sr : oDev.sampleRates)
    {
      srAvail[sr]++;
    }
  }

  int selIdx{-1}, sIdx{0};
  for (auto [sr, ct] : srAvail)
  {
    if (ct == 2)
    {
      [sampleRateSelection addItemWithTitle:[NSString stringWithFormat:@"%d", sr]];
      if ((int)sr == (int)csr) selIdx = sIdx;

      sIdx++;
    }
  }
  if (selIdx >= 0) [sampleRateSelection selectItemAtIndex:selIdx];
}

@end
