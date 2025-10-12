#import "AppDelegate.h"

#include <AVFoundation/AVFoundation.h>

#include <map>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"

#include "detail/clap/fsutil.h"

@interface ClapWrapperAppDelegate ()

@end

@interface AudioSettingsWindow : NSWindow
{
  NSPopUpButton *outputSelection, *inputSelection, *sampleRateSelection;
  std::vector<RtAudio::DeviceInfo> outDevices, inDevices;
}

- (void)setupContents;
- (void)resetSampleRateSelection;

@end

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
    standaloneHost->activatePlugin(standaloneHost->currentSampleRate, 1,
                                   standaloneHost->currentBufferSize * 2);
    standaloneHost->running = true;
  }
}

- (void)doSetup
{
  // Insert code here to initialize your application
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
    return;
  }
  self.requestCallbackTimer = [NSTimer timerWithTimeInterval:0.08
                                                      target:self
                                                    selector:@selector(timerCallback:)
                                                    userInfo:nil
                                                     repeats:YES];
  auto *runLoop = [NSRunLoop currentRunLoop];
  [runLoop addTimer:self.requestCallbackTimer forMode:NSDefaultRunLoopMode];
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

  [[self window] orderFrontRegardless];
  [[self window] setDelegate:self];

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize =
      [self](uint32_t w, uint32_t h)
  {
    NSSize sz;
    sz.width = w;
    sz.height = h;
    [[self window] setContentSize:sz];
    return false;
  };

  if (plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto p = plugin->_plugin;
    if (!ui->is_api_supported(p, CLAP_WINDOW_API_COCOA, false))
      LOGINFO("[WARNING] GUI API not supported");

    ui->create(p, CLAP_WINDOW_API_COCOA, false);
    ui->set_scale(p, 1);

    uint32_t w, h;
    ui->get_size(p, &w, &h);
    if (ui->can_resize(p))
    {
      ui->adjust_size(p, &w, &h);
    }

    NSView *view = [[self window] contentView];

    NSSize sz;
    sz.width = w;
    sz.height = h;
    [[self window] setContentSize:sz];

    clap_window win;
    win.api = CLAP_WINDOW_API_COCOA;
    win.cocoa = view;
    ui->set_parent(p, &win);
    ui->show(p);
  }

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = [](auto &s)
  {
    NSLog(@"Error Reported: %s", s.c_str());
    @autoreleasepool
    {
      NSAlert *alert = [[NSAlert alloc] init];
      [alert setMessageText:@"Unable to configure audio"];
      [alert setInformativeText:[[NSString alloc] initWithUTF8String:s.c_str()]];
      [alert addButtonWithTitle:@"OK"];
      [alert runModal];
    }
  };

  freeaudio::clap_wrapper::standalone::mainStartAudio();
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
  [NSApp activateIgnoringOtherApps:YES];
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

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  LOGDETAIL("Application terminating");
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = nullptr;
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize = nullptr;

  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  if (plugin && plugin->_ext._gui)
  {
    plugin->_ext._gui->hide(plugin->_plugin);
    plugin->_ext._gui->destroy(plugin->_plugin);
  }

  [self.requestCallbackTimer invalidate];
  self.requestCallbackTimer = nil;

  freeaudio::clap_wrapper::standalone::mainFinish();
}

- (IBAction)openAudioSettingsWindow:(id)sender
{
  @autoreleasepool
  {
    NSRect windowRect = NSMakeRect(0, 0, 400, 360);

    auto *window = [[AudioSettingsWindow alloc]
        initWithContentRect:windowRect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];

    [window setupContents];

    // Center the window and make it key window and front.
    [window center];
    [window makeKeyAndOrderFront:nil];
  }
}

- (void)windowDidResize:(NSNotification *)notification
{
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  if (plugin && plugin->_ext._gui)
  {
    auto canRS = plugin->_ext._gui->can_resize(plugin->_plugin);
    if (canRS)
    {
      auto w = [self window];
      auto f = [w frame];
      auto cr = [w contentRectForFrameRect:f];
      plugin->_ext._gui->set_size(plugin->_plugin, cr.size.width, cr.size.height);
    }
  }
}

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize
{
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  if (plugin && plugin->_ext._gui)
  {
    auto w = [self window];
    auto f = [w frame];
    f.size = frameSize;
    auto cr = [w contentRectForFrameRect:f];

    auto canRS = plugin->_ext._gui->can_resize(plugin->_plugin);
    if (!canRS)
    {
      uint32_t w, h;
      plugin->_ext._gui->get_size(plugin->_plugin, &w, &h);
      cr.size.width = w;
      cr.size.height = h;
    }
    else
    {
      uint32_t w = frameSize.width, h = frameSize.height;
      plugin->_ext._gui->adjust_size(plugin->_plugin, &w, &h);
      cr.size.width = w;
      cr.size.height = h;
    }
    auto fr = [w frameRectForContentRect:cr];
    frameSize = fr.size;
  }
  return frameSize;
}

- (IBAction)streamWrapperFileAs:(id)sender
{
  NSSavePanel *savePanel = [NSSavePanel savePanel];
  [savePanel setNameFieldStringValue:@"Untitled"];  //

  if ([savePanel runModal] == NSModalResponseOK)
  {
    NSURL *documentURL = [savePanel URL];
    auto fsp = fs::path{[[documentURL path] UTF8String]};
    auto fn = fsp.replace_extension(".cwstream");

    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();

    try
    {
      standaloneHost->saveStandaloneAndPluginSettings(fn.parent_path(), fn.filename());
    }
    catch (const fs::filesystem_error &e)
    {
      NSAlert *alert = [[NSAlert alloc] init];
      [alert setMessageText:@"Unable to save file"];
      [alert setInformativeText:[[NSString alloc] initWithUTF8String:e.what()]];
      [alert addButtonWithTitle:@"OK"];
      [alert runModal];
    }
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
    }
    catch (const fs::filesystem_error &e)
    {
      NSAlert *alert = [[NSAlert alloc] init];
      [alert setMessageText:@"Unable to open file"];
      [alert setInformativeText:[[NSString alloc] initWithUTF8String:e.what()]];
      [alert addButtonWithTitle:@"OK"];
      [alert runModal];
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
