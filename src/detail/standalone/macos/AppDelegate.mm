#import "AppDelegate.h"

#include <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <string>
#include <vector>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"

#include "detail/clap/fsutil.h"

@interface ClapWrapperAppDelegate ()
- (void)bringWindowToFront;
@end

namespace
{
namespace sa = freeaudio::clap_wrapper::standalone;

// a checkbox in one of the settings window's midi port lists
struct MidiPortRow
{
  // the port's id, or for a port saved but not plugged in, its saved name
  std::string reference;
  NSButton *check;  // owned by the window's views
  bool wasOn;       // when the window opened
};

// a stream's setup as the host holds it: what runs, or what a window shows
struct AudioSetup
{
  bool running{false};
  std::string inputId, outputId;
  bool useInput{false}, useOutput{false};
  int32_t rate{0};
  uint32_t frames{0};
  sa::AudioChannelChoice inputChannels, outputChannels;
};

AudioSetup runningSetup()
{
  const auto *host{sa::getStandaloneHost()};
  AudioSetup setup;
  setup.running = host->isAudioRunning();
  setup.inputId = host->audioInputDevice;
  setup.outputId = host->audioOutputDevice;
  setup.useInput = host->audioInputUsed;
  setup.useOutput = host->audioOutputUsed;
  setup.rate = host->currentSampleRate;
  setup.frames = host->currentBufferSize;
  setup.inputChannels = {host->deviceInputFirstChannel, host->deviceInputChannels, {}};
  setup.outputChannels = {host->deviceOutputFirstChannel, host->deviceOutputChannels, {}};
  return setup;
}

// whether the stream running now is this one: then opening it again would only drop out
// the audio
bool runsAs(const AudioSetup &shown)
{
  const auto *host{sa::getStandaloneHost()};
  const auto now{runningSetup()};
  return now.running && shown.rate == now.rate && (shown.frames == 0 || shown.frames == now.frames) &&
         shown.useOutput == (host->currentOutputChannels > 0) &&
         (!shown.useOutput ||
          (shown.outputId == now.outputId && shown.outputChannels.first == now.outputChannels.first &&
           shown.outputChannels.count == now.outputChannels.count)) &&
         shown.useInput == (host->currentInputChannels > 0) &&
         (!shown.useInput ||
          (shown.inputId == now.inputId && shown.inputChannels.first == now.inputChannels.first &&
           shown.inputChannels.count == now.inputChannels.count));
}

// opens a stream on this setup; false, with the error already shown, if it does not open
bool openSetup(const AudioSetup &setup)
{
  auto *host{sa::getStandaloneHost()};
  // the size to ask for; the stream reads back what the device granted
  if (setup.frames > 0) host->currentBufferSize = setup.frames;
  host->deviceInputFirstChannel = setup.inputChannels.first;
  host->deviceInputChannels = std::max<uint32_t>(setup.inputChannels.count, 1);
  host->deviceOutputFirstChannel = setup.outputChannels.first;
  host->deviceOutputChannels = std::max<uint32_t>(setup.outputChannels.count, 1);
  host->startAudioThreadOn(setup.inputId, host->deviceInputChannels, setup.useInput, setup.outputId,
                           host->deviceOutputChannels, setup.useOutput, setup.rate);
  return host->isAudioRunning();
}

// what StandaloneHost::screenAppearance says, on the settings window or on one of the
// standalone's alerts. nil appearance follows the system
void applyScreenAppearance(NSWindow *window)
{
  if (@available(macOS 10.14, *))
  {
    NSAppearance *appearance{nil};
    switch (freeaudio::clap_wrapper::standalone::getStandaloneHost()->screenAppearance)
    {
      case CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT:
        appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
        break;
      case CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK:
        appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        break;
      default:
        break;
    }
    [window setAppearance:appearance];
  }
}
}  // namespace

@interface AudioSettingsFlippedView : NSView
@end

@implementation AudioSettingsFlippedView
- (BOOL)isFlipped
{
  return YES;
}
@end

@interface AudioSettingsWindow : NSWindow
{
  NSPopUpButton *outputSelection, *inputSelection, *sampleRateSelection, *bufferSizeSelection;
  std::vector<sa::AudioDevice> outDevices, inDevices;

  // which of the selected device's channels the plugin's main ports take
  NSPopUpButton *outputChannelSelection, *inputChannelSelection;
  std::vector<sa::AudioChannelChoice> outChoices, inChoices;
  NSGridRow *inputChannelRow;

  // a list nobody touched leaves its settings as they were, which for the inputs may be
  // "every port", including any plugged in later
  std::vector<MidiPortRow> midiInRows, midiOutRows;
  NSScrollView *midiInList, *midiOutList;
  // the input list's "every port" from the settings, for ports plugged in while it is open
  bool midiInAllOn;

  // the optional virtual ports, and whether each was asked for when the window opened
  NSButton *virtualInCheck, *virtualOutCheck;
  bool virtualInWasOn, virtualOutWasOn;

  // OK saved what the window shows; Cancel and the close button did not
  bool applied;
  bool closeReported;

  // what ran when the window opened, which Cancel and the close button go back to once an
  // audio menu has opened another stream (audioOpenedHere)
  AudioSetup audioWhenOpened;
  bool audioOpenedHere;

  // OK is off, and the note says why, while the input and output have no sample rate in
  // common: one stream runs both at one rate, so the devices could not be opened together
  NSButton *okButton;
  NSGridRow *noRateRow;

  // under the input menu, while it matters: that a bluetooth microphone takes its whole
  // device down to telephone quality, or that the default input is a microphone and was not
  // opened
  NSTextField *inputNote;
  NSGridRow *inputNoteRow;

  // beside the buffer size: the running stream's latency each way, as its devices report
  // it (the buffer not included), while the window shows what runs
  NSTextField *latencyNote;

  // the window's own appearance menu, offered while the plugin has not set the
  // appearance itself (StandaloneHost::screenAppearanceFromPlugin)
  NSPopUpButton *appearanceSelection;
  NSGridRow *appearanceRow, *appearanceRuleRow;
  bool offerAppearance;
  uint32_t appearanceWhenOpened;
}

- (void)setupContents;
- (void)fillDeviceMenusSelectingOutput:(const std::string &)output input:(const std::string &)input;
- (void)devicesChanged;
- (void)resetChannelSelectionsPreferringOutput:(uint32_t)outputFirst input:(uint32_t)inputFirst;
- (sa::AudioChannelChoice)shownChoice:(bool)input;
- (void)resetSampleRateSelectionPreferring:(uint32_t)rate;
- (void)resetBufferSizeSelectionPreferring:(uint32_t)frames;
- (void)updateBufferSizeTitles;
- (void)updateInputNote;
- (void)updateLatencyNote;
- (void)applyAppearance;
- (AudioSetup)shownSetup;
- (bool)openShownAudio;
- (void)showRunningSetup;
- (void)onAudioMenuChanged:(id)sender;
- (void)onSampleRateChanged:(id)sender;

@end

// the settings window while it is open. not retained: it is released when it closes
// (isReleasedWhenClosed), and clears this before that
static AudioSettingsWindow *openAudioSettings{nil};

// an audio error alert is up. the polling timer and the host go on inside its modal loop,
// and a stream opened again on a device just plugged back in can stop once more: that is
// the failure already on screen, and one alert at a time shows it
static bool showingAudioError{false};

@implementation ClapWrapperAppDelegate

- (void)timerCallback:(NSTimer *)instance
{
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();

  // a stream that stopped on its own, devices that came or went
  standaloneHost->pollAudioEvents();
  // which may have stopped the stream the window's latency describes, or opened another
  [openAudioSettings updateLatencyNote];

  if (standaloneHost->callbackRequested.exchange(false))
  {
    auto *plugin = freeaudio::clap_wrapper::standalone::getMainPlugin()->_plugin;
    plugin->on_main_thread(plugin);
  }

  // an inactive plugin has nothing to restart
  if (standaloneHost->restartRequested.exchange(false) && standaloneHost->isActive)
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

  // before the plugin is created: a plugin asks supports_audio_midi_configuration_screen
  // from init() or from its GUI's create(), and both run inside mainCreatePlugin and the
  // block below it
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onShowSettingsScreen = [self]()
  {
    [self openAudioSettingsWindow:nil];
    return true;
  };
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onScreenAppearanceChanged = []()
  {
    if (openAudioSettings) [openAudioSettings applyAppearance];
  };

  auto plugin =
      freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, (char **)argv);

  // the urls the standalone was opened with go to the plugin once this is done, whichever
  // way it ends: with the gui shown, with none, or with one that could not be
  struct BeginUrlDelivery
  {
    ~BeginUrlDelivery()
    {
      freeaudio::clap_wrapper::standalone::getStandaloneHost()->beginUrlDelivery();
    }
  } deliverUrlsWhenDone;

  [[self window] orderFrontRegardless];
  [[self window] setDelegate:self];

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize =
      [self](uint32_t w, uint32_t h)
  {
    NSSize sz;
    sz.width = w;
    sz.height = h;
    [[self window] setContentSize:sz];
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
      @autoreleasepool
      {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Plugin Error"];
        [alert setInformativeText:@"The plugin failed to create its user interface."];
        [alert addButtonWithTitle:@"OK"];
        applyScreenAppearance([alert window]);
        [alert runModal];
      }
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
      @autoreleasepool
      {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Plugin Error"];
        [alert setInformativeText:sizeError];
        [alert addButtonWithTitle:@"OK"];
        applyScreenAppearance([alert window]);
        [alert runModal];
      }
      ui->destroy(p);
      return;
    }

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
    if (!ui->set_parent(p, &win))
    {
      LOGINFO("[ERROR] Plugin GUI set_parent() failed");
      @autoreleasepool
      {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Plugin Error"];
        [alert
            setInformativeText:
                @"The plugin failed to embed its user interface. Please contact the plugin developer."];
        [alert addButtonWithTitle:@"OK"];
        applyScreenAppearance([alert window]);
        [alert runModal];
      }
      return;
    }
    ui->show(p);
  }

  // shown from a turn of the run loop of its own: the host reports from inside its own
  // calls, and from the timer that polls for a device coming back, which a modal alert run
  // there would hold up until it is dismissed
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = [](auto &s)
  {
    NSLog(@"Error Reported: %s", s.c_str());
    NSString *text = [[[NSString alloc] initWithUTF8String:s.c_str()] autorelease];
    dispatch_async(dispatch_get_main_queue(), ^{
      if (showingAudioError) return;
      showingAudioError = true;
      @autoreleasepool
      {
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Unable to configure audio"];
        [alert setInformativeText:text];
        [alert addButtonWithTitle:@"OK"];
        applyScreenAppearance([alert window]);
        [alert runModal];
      }
      showingAudioError = false;
    });
  };

  // the settings window, while it is open, follows devices and ports coming and going
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onAudioDevicesChanged = []()
  { [openAudioSettings devicesChanged]; };

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

// a link in a scheme the standalone was built to open: clicked while it runs, or the one
// it was launched for, which arrives before applicationDidFinishLaunching - long before
// doSetup has made the plugin - and is held by the host until then
- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls
{
  std::vector<std::string> own;
  for (NSURL *url in urls)
  {
    const char *text = [[url absoluteString] UTF8String];
    if (text && freeaudio::clap_wrapper::standalone::isOwnUrl(text)) own.emplace_back(text);
  }
  if (own.empty()) return;

  // launching, the window comes up in front anyway
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  if (standaloneHost->urlDeliveryBegun) [self bringWindowToFront];

  for (const auto &url : own) standaloneHost->openUrl(url);
}

- (void)bringWindowToFront
{
  if ([[self window] isMiniaturized]) [[self window] deminiaturize:nil];
  [[self window] makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
  return true;
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  LOGDETAIL("Application terminating");
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError = nullptr;
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onAudioDevicesChanged = nullptr;
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize = nullptr;
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onShowSettingsScreen = nullptr;

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
  // one window at a time, as show_audio_midi_configuration_screen promises
  if (openAudioSettings)
  {
    [openAudioSettings makeKeyAndOrderFront:nil];
    return;
  }

  @autoreleasepool
  {
    // sized to its contents by setupContents
    NSRect windowRect = NSMakeRect(0, 0, 480, 400);

    auto *window = [[AudioSettingsWindow alloc]
        initWithContentRect:windowRect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                            NSWindowStyleMaskMiniaturizable
                    backing:NSBackingStoreBuffered
                      defer:NO];

    [window setupContents];
    openAudioSettings = window;

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
      uint32_t w = cr.size.width, h = cr.size.height;
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
      applyScreenAppearance([alert window]);
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
      applyScreenAppearance([alert window]);
      [alert runModal];
    }
  }
}

// whether the next launch gets this session's state back; saved with the audio and midi setup
- (IBAction)toggleRestoreLastSession:(id)sender
{
  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  if (!standaloneHost) return;

  standaloneHost->setPluginStatePersistence(standaloneHost->restoresLastSession()
                                                ? CLAP_WRAPPER_STANDALONE_STATE_PERSIST_NONE
                                                : CLAP_WRAPPER_STANDALONE_STATE_PERSIST_SESSION);
}

// Restore Last Session shows the choice in effect, which the plugin may have changed since
- (BOOL)validateMenuItem:(NSMenuItem *)menuItem
{
  if ([menuItem action] == @selector(toggleRestoreLastSession:))
  {
    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    const bool restores{standaloneHost && standaloneHost->restoresLastSession()};
    [menuItem setState:restores ? NSControlStateValueOn : NSControlStateValueOff];
    return standaloneHost && standaloneHost->settingsFolder.has_value();
  }
  return YES;
}

// back to the plugin's state as it was at launch, before the saved one was loaded over it
- (IBAction)resetState:(id)sender
{
  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();
  if (!standaloneHost || !plugin || !standaloneHost->settingsFolder.has_value()) return;

  auto loadPath = *standaloneHost->settingsFolder;

  try
  {
    if (fs::exists(loadPath / "defaults.clapwrapper"))
    {
      standaloneHost->tryLoadStandaloneAndPluginSettings(loadPath, "defaults.clapwrapper");
    }
  }
  catch (const fs::filesystem_error &e)
  {
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Unable to reset state"];
    [alert setInformativeText:[[NSString alloc] initWithUTF8String:e.what()]];
    [alert addButtonWithTitle:@"OK"];
    applyScreenAppearance([alert window]);
    [alert runModal];
  }
}

@end

namespace
{
// the width of the device menus and port lists
constexpr CGFloat controlWidth{320};
// from the window's edges, and so clear of its rounded corners
constexpr CGFloat windowMargin{20};
// port lists show this many rows and scroll the rest
constexpr size_t visiblePortRows{5};

NSString *pluginDisplayName()
{
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();
  if (plugin && plugin->_plugin && plugin->_plugin->desc && plugin->_plugin->desc->name &&
      *plugin->_plugin->desc->name)
    return [NSString stringWithUTF8String:plugin->_plugin->desc->name];
  return [NSString stringWithUTF8String:OUTPUT_NAME];
}

NSTextField *makeLabel(NSString *s)
{
  return [NSTextField labelWithString:s];
}

NSTextField *makeNote(NSString *s, NSColor *color)
{
  NSTextField *note = [NSTextField wrappingLabelWithString:s];
  [note setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
  [note setTextColor:color];
  [note setPreferredMaxLayoutWidth:controlWidth];
  return note;
}

NSBox *makeRule()
{
  NSBox *rule = [[[NSBox alloc] init] autorelease];
  [rule setBoxType:NSBoxSeparator];
  return rule;
}

bool contains(const std::vector<std::string> &names, const std::string &name)
{
  return std::find(names.begin(), names.end(), name) != names.end();
}

// one checkbox per port. ports saved but not plugged in now are listed too, marked, so
// that unticking one is a way to forget it. a row already there keeps its state, so that a
// port coming or going while the window is open changes nothing else
void makePortRows(std::vector<MidiPortRow> &rows, const std::vector<sa::MidiPort> &ports,
                  const std::vector<std::string> &onIds, const std::vector<std::string> &absentNames,
                  bool allOn)
{
  const auto previous{rows};
  rows.clear();
  auto addRow = [&](const std::string &reference, const std::string &name, bool connected, bool isOn)
  {
    bool wasOn{isOn};
    for (const auto &row : previous)
    {
      if (row.reference != reference) continue;
      isOn = [row.check state] == NSControlStateValueOn;
      wasOn = row.wasOn;
    }
    NSString *title = [NSString stringWithUTF8String:name.c_str()];
    if (!connected) title = [title stringByAppendingString:@" (not connected)"];
    NSButton *check = [NSButton checkboxWithTitle:title target:nil action:nil];
    [check setState:isOn ? NSControlStateValueOn : NSControlStateValueOff];
    [[check cell] setLineBreakMode:NSLineBreakByTruncatingTail];
    rows.push_back({reference, check, wasOn});
  };
  for (const auto &port : ports) addRow(port.id, port.name, true, allOn || contains(onIds, port.id));
  if (!allOn)
    for (const auto &name : absentNames) addRow(name, name, false, true);
}

// the rows in a scroll view, made here or given to fill again
NSScrollView *layoutPortList(NSScrollView *list, std::vector<MidiPortRow> &rows, NSString *emptyText)
{
  NSTextField *empty = nil;
  if (rows.empty())
  {
    empty = makeLabel(emptyText);
    [empty setTextColor:[NSColor secondaryLabelColor]];
  }

  // laid out by hand inside the scroll view, which is simpler there than constraints
  const CGFloat inset{4}, gap{2};
  const CGFloat rowHeight =
      rows.empty() ? [empty fittingSize].height : [rows.front().check fittingSize].height;
  const size_t shown = std::clamp(rows.size(), (size_t)1, visiblePortRows);
  const size_t total = std::max(rows.size(), (size_t)1);

  // a list that scrolls shows half a row more, so that it plainly does
  const bool scrolls = rows.size() > visiblePortRows;
  const CGFloat shownRows = shown + (scrolls ? 0.5 : 0);
  const NSSize frame =
      NSMakeSize(controlWidth, 2 * inset + shownRows * rowHeight + (shownRows - 1) * gap + 2);
  const NSSize content = [NSScrollView contentSizeForFrameSize:frame
                                       horizontalScrollerClass:nil
                                         verticalScrollerClass:scrolls ? [NSScroller class] : nil
                                                    borderType:NSBezelBorder
                                                   controlSize:NSControlSizeRegular
                                                 scrollerStyle:[NSScroller preferredScrollerStyle]];

  const CGFloat docHeight = 2 * inset + total * rowHeight + (total - 1) * gap;
  NSView *doc = [[[AudioSettingsFlippedView alloc]
      initWithFrame:NSMakeRect(0, 0, content.width, std::max(docHeight, content.height))] autorelease];
  if (empty)
  {
    [empty setFrame:NSMakeRect(2 * inset, inset, content.width - 4 * inset, rowHeight)];
    [doc addSubview:empty];
  }
  for (size_t i = 0; i < rows.size(); ++i)
  {
    [rows[i].check setFrame:NSMakeRect(2 * inset, inset + i * (rowHeight + gap),
                                       content.width - 4 * inset, rowHeight)];
    [doc addSubview:rows[i].check];
  }

  if (!list)
  {
    list =
        [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, frame.width, frame.height)] autorelease];
    [list setBorderType:NSBezelBorder];
    [list setAutohidesScrollers:YES];
    [list setDrawsBackground:YES];
    [list setBackgroundColor:[NSColor controlBackgroundColor]];
    [list setTranslatesAutoresizingMaskIntoConstraints:NO];
  }
  else
  {
    // its own size, from before
    NSMutableArray *own = [NSMutableArray array];
    for (NSLayoutConstraint *c in [list constraints])
      if ([c firstItem] == list && [c secondItem] == nil) [own addObject:c];
    [NSLayoutConstraint deactivateConstraints:own];
  }
  [list setHasVerticalScroller:scrolls];
  [list setDocumentView:doc];
  [[list widthAnchor] constraintEqualToConstant:frame.width].active = YES;
  [[list heightAnchor] constraintEqualToConstant:frame.height].active = YES;
  return list;
}

bool listChanged(const std::vector<MidiPortRow> &rows)
{
  for (const auto &row : rows)
    if (([row.check state] == NSControlStateValueOn) != row.wasOn) return true;
  return false;
}

std::vector<std::string> ticked(const std::vector<MidiPortRow> &rows)
{
  std::vector<std::string> references;
  for (const auto &row : rows)
    if ([row.check state] == NSControlStateValueOn) references.push_back(row.reference);
  return references;
}
}  // namespace

@implementation AudioSettingsWindow

- (void)setupContents
{
  @autoreleasepool
  {
    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    NSString *pluginName = pluginDisplayName();
    audioWhenOpened = runningSetup();
    audioOpenedHere = false;

    [self setTitle:@"Audio/MIDI Settings"];

    NSTextField *heading = makeLabel([NSString stringWithFormat:@"Audio/MIDI Setup for %@", pluginName]);
    [heading setFont:[NSFont systemFontOfSize:15 weight:NSFontWeightSemibold]];
    [heading setLineBreakMode:NSLineBreakByTruncatingTail];
    NSBox *headingRule = makeRule();

    // audio
    outputSelection = [[[NSPopUpButton alloc] init] autorelease];
    inputSelection = [[[NSPopUpButton alloc] init] autorelease];
    sampleRateSelection = [[[NSPopUpButton alloc] init] autorelease];
    bufferSizeSelection = [[[NSPopUpButton alloc] init] autorelease];
    for (NSPopUpButton *menu : {outputSelection, inputSelection})
    {
      [[menu cell] setLineBreakMode:NSLineBreakByTruncatingTail];
      [[menu widthAnchor] constraintEqualToConstant:controlWidth].active = YES;
    }
    [[sampleRateSelection widthAnchor] constraintEqualToConstant:120].active = YES;
    [[bufferSizeSelection widthAnchor] constraintEqualToConstant:120].active = YES;
    [sampleRateSelection setAction:@selector(onSampleRateChanged:)];
    [sampleRateSelection setTarget:self];
    [bufferSizeSelection setAction:@selector(onAudioMenuChanged:)];
    [bufferSizeSelection setTarget:self];

    latencyNote = makeLabel(@"");
    [latencyNote setFont:[NSFont systemFontOfSize:[NSFont smallSystemFontSize]]];
    [latencyNote setTextColor:[NSColor secondaryLabelColor]];
    NSStackView *bufferSizeCell = [NSStackView stackViewWithViews:@[ bufferSizeSelection, latencyNote ]];
    [bufferSizeCell setAlignment:NSLayoutAttributeFirstBaseline];
    [bufferSizeCell setSpacing:12];

    // the input is the default's even with no input open, and showing that device would
    // have OK open it
    [self
        fillDeviceMenusSelectingOutput:standaloneHost->audioOutputDevice
                                 input:standaloneHost->audioInputUsed ? standaloneHost->audioInputDevice
                                                                      : std::string{}];
    [outputSelection setAction:@selector(onSourceMenuChanged:)];
    [outputSelection setTarget:self];
    [inputSelection setAction:@selector(onSourceMenuChanged:)];
    [inputSelection setTarget:self];

    outputChannelSelection = [[[NSPopUpButton alloc] init] autorelease];
    inputChannelSelection = [[[NSPopUpButton alloc] init] autorelease];
    for (NSPopUpButton *menu : {outputChannelSelection, inputChannelSelection})
    {
      [[menu cell] setLineBreakMode:NSLineBreakByTruncatingTail];
      [[menu widthAnchor] constraintEqualToConstant:controlWidth].active = YES;
      [menu setAction:@selector(onAudioMenuChanged:)];
      [menu setTarget:self];
    }
    // a mono main input takes one channel, anything else a pair
    NSString *inputChannelLabel =
        standaloneHost->mainBusChannels(true) == 1 ? @"Input Channel:" : @"Input Channels:";

    NSButton *defaultButton = [NSButton buttonWithTitle:@"Reset to System Default"
                                                 target:self
                                                 action:@selector(defaultButtonPressed:)];

    NSTextField *noRateNote =
        makeNote(@"These devices have no sample rate in common.", [NSColor systemRedColor]);
    inputNote = makeNote(@"", [NSColor secondaryLabelColor]);

    // midi: a direction the plugin has no note port for gets no section at all
    const auto &settings = standaloneHost->settings;
    const bool takesMidi = standaloneHost->hasMIDIInput || standaloneHost->hasClapNoteInput;
    const bool sendsMidi = standaloneHost->createsMidiOutput;

    std::vector<std::string> onIds, absentNames;
    midiInAllOn = settings.midiBindAllPorts;
    standaloneHost->savedMidiSelection(true, onIds, absentNames);
    makePortRows(midiInRows, standaloneHost->getMidiInputPorts(), onIds, absentNames, midiInAllOn);
    midiInList = layoutPortList(nil, midiInRows, @"No MIDI inputs found");
    standaloneHost->savedMidiSelection(false, onIds, absentNames);
    makePortRows(midiOutRows, standaloneHost->getMidiOutputPorts(), onIds, absentNames, false);
    midiOutList = layoutPortList(nil, midiOutRows, @"No MIDI outputs found");

    // offered where the midi api has virtual ports, which on macos is every version
    const bool virtualPorts = standaloneHost->supportsVirtualMidiPorts();
    auto makeVirtualCheck = [&](bool input, bool isOn)
    {
      NSString *name =
          [NSString stringWithUTF8String:standaloneHost->virtualMidiPortName(input).c_str()];
      NSString *title = [NSString
          stringWithFormat:input ? @"Virtual input \u201C%@\u201D" : @"Virtual output \u201C%@\u201D",
                           name];
      NSButton *check = [NSButton checkboxWithTitle:title target:nil action:nil];
      [check setState:isOn ? NSControlStateValueOn : NSControlStateValueOff];
      [check setToolTip:input ? @"A MIDI destination other applications can send to"
                              : @"A MIDI source other applications can receive from"];
      return check;
    };
    virtualInWasOn = settings.midiVirtualInput;
    virtualOutWasOn = settings.midiVirtualOutput;
    virtualInCheck = makeVirtualCheck(true, virtualInWasOn);
    virtualOutCheck = makeVirtualCheck(false, virtualOutWasOn);

    NSTextField *midiInLabel = makeLabel(@"MIDI Inputs:");
    NSTextField *midiOutLabel = makeLabel(@"MIDI Outputs:");
    NSView *none = [NSGridCell emptyContentView];
    NSBox *sectionRule = makeRule();

    // appearance
    appearanceSelection = [[[NSPopUpButton alloc] init] autorelease];
    [[appearanceSelection widthAnchor] constraintEqualToConstant:120].active = YES;
    const auto appearances{freeaudio::clap_wrapper::standalone::supportedScreenAppearances()};
    const std::pair<uint32_t, NSString *> appearanceItems[] = {
        {CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO, @"Auto"},
        {CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT, @"Light"},
        {CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK, @"Dark"}};
    for (const auto &[appearance, title] : appearanceItems)
    {
      if (!(appearances & (1u << appearance))) continue;
      [appearanceSelection addItemWithTitle:title];
      [[appearanceSelection lastItem] setTag:appearance];
    }
    [appearanceSelection setAction:@selector(onAppearanceChanged:)];
    [appearanceSelection setTarget:self];
    offerAppearance = [appearanceSelection numberOfItems] > 1;
    appearanceWhenOpened = standaloneHost->screenAppearance;
    NSBox *appearanceRule = makeRule();

    NSGridView *grid = [NSGridView gridViewWithViews:@[
      @[ makeLabel(@"Output:"), outputSelection ],
      @[ makeLabel(@"Output Channels:"), outputChannelSelection ],
      @[ makeLabel(@"Input:"), inputSelection ],
      @[ makeLabel(inputChannelLabel), inputChannelSelection ],
      @[ none, inputNote ],
      @[ makeLabel(@"Sample Rate:"), sampleRateSelection ],
      @[ makeLabel(@"Buffer Size:"), bufferSizeCell ],
      @[ none, defaultButton ],
      @[ none, noRateNote ],
      @[ sectionRule, none ],
      @[ midiInLabel, midiInList ],
      @[ none, virtualInCheck ],
      @[ midiOutLabel, midiOutList ],
      @[ none, virtualOutCheck ],
      @[ appearanceRule, none ],
      @[ makeLabel(@"Appearance:"), appearanceSelection ],
    ]];
    [grid setRowSpacing:8];
    [grid setColumnSpacing:8];
    [grid setRowAlignment:NSGridRowAlignmentFirstBaseline];
    [[grid columnAtIndex:0] setXPlacement:NSGridCellPlacementTrailing];

    auto rowOf = [grid](NSView *v) { return [[grid cellForView:v] row]; };

    inputChannelRow = rowOf(inputChannelSelection);
    noRateRow = rowOf(noRateNote);
    [noRateRow setHidden:YES];
    inputNoteRow = rowOf(inputNote);
    [inputNoteRow setHidden:YES];
    [rowOf(sectionRule) setHidden:!takesMidi && !sendsMidi];
    [rowOf(midiInList) setHidden:!takesMidi];
    [rowOf(midiOutList) setHidden:!sendsMidi];
    [rowOf(virtualInCheck) setHidden:!takesMidi || !virtualPorts];
    [rowOf(virtualOutCheck) setHidden:!sendsMidi || !virtualPorts];

    for (NSBox *rule : {sectionRule, appearanceRule})
    {
      NSGridRow *ruleRow = rowOf(rule);
      [ruleRow mergeCellsInRange:NSMakeRange(0, 2)];
      [ruleRow setRowAlignment:NSGridRowAlignmentNone];
      [ruleRow setYPlacement:NSGridCellPlacementCenter];
      [ruleRow setTopPadding:6];
      [ruleRow setBottomPadding:6];
      [[ruleRow cellAtIndex:0] setXPlacement:NSGridCellPlacementFill];
    }
    appearanceRow = rowOf(appearanceSelection);
    appearanceRuleRow = rowOf(appearanceRule);

    // a list has no baseline: its label is centred on the list's first row instead,
    // which puts the two lines of text level
    auto alignListLabel = [&](NSView *label, NSView *list, const std::vector<MidiPortRow> &rows)
    {
      NSGridRow *row = rowOf(list);
      [row setRowAlignment:NSGridRowAlignmentNone];
      [row setYPlacement:NSGridCellPlacementTop];

      NSGridCell *labelCell = [grid cellForView:label];
      const CGFloat firstRow =
          rows.empty() ? [label fittingSize].height : [rows.front().check fittingSize].height;
      [labelCell setYPlacement:NSGridCellPlacementNone];
      [labelCell setCustomPlacementConstraints:@[ [[label centerYAnchor]
                                                   constraintEqualToAnchor:[list topAnchor]
                                                                  constant:1 + 4 + firstRow / 2] ]];
    };
    alignListLabel(midiInLabel, midiInList, midiInRows);
    alignListLabel(midiOutLabel, midiOutList, midiOutRows);

    // buttons
    NSButton *cancelButton = [NSButton buttonWithTitle:@"Cancel"
                                                target:self
                                                action:@selector(cancelButtonPressed:)];
    [cancelButton setKeyEquivalent:@"\033"];
    okButton = [NSButton buttonWithTitle:@"OK" target:self action:@selector(okButtonPressed:)];
    [okButton setKeyEquivalent:@"\r"];

    NSView *content = [self contentView];
    for (NSView *v : {(NSView *)heading, (NSView *)headingRule, (NSView *)grid, (NSView *)cancelButton,
                      (NSView *)okButton})
    {
      [v setTranslatesAutoresizingMaskIntoConstraints:NO];
      [content addSubview:v];
    }

    [NSLayoutConstraint activateConstraints:@[
      [[heading topAnchor] constraintEqualToAnchor:[content topAnchor] constant:windowMargin - 4],
      [[heading leadingAnchor] constraintEqualToAnchor:[content leadingAnchor] constant:windowMargin],
      [[heading trailingAnchor] constraintLessThanOrEqualToAnchor:[content trailingAnchor]
                                                         constant:-windowMargin],

      [[headingRule topAnchor] constraintEqualToAnchor:[heading bottomAnchor] constant:8],
      [[headingRule leadingAnchor] constraintEqualToAnchor:[content leadingAnchor]
                                                  constant:windowMargin],
      [[headingRule trailingAnchor] constraintEqualToAnchor:[content trailingAnchor]
                                                   constant:-windowMargin],

      [[grid topAnchor] constraintEqualToAnchor:[headingRule bottomAnchor] constant:16],
      [[grid leadingAnchor] constraintEqualToAnchor:[content leadingAnchor] constant:windowMargin],
      [[grid trailingAnchor] constraintEqualToAnchor:[content trailingAnchor] constant:-windowMargin],

      [[okButton topAnchor] constraintEqualToAnchor:[grid bottomAnchor] constant:24],
      [[okButton trailingAnchor] constraintEqualToAnchor:[content trailingAnchor]
                                                constant:-windowMargin],
      [[okButton bottomAnchor] constraintEqualToAnchor:[content bottomAnchor] constant:-windowMargin],
      [[okButton widthAnchor] constraintGreaterThanOrEqualToConstant:80],

      [[cancelButton trailingAnchor] constraintEqualToAnchor:[okButton leadingAnchor] constant:-12],
      [[cancelButton centerYAnchor] constraintEqualToAnchor:[okButton centerYAnchor]],
      [[cancelButton widthAnchor] constraintEqualToAnchor:[okButton widthAnchor]],
    ]];

    [self resetSampleRateSelectionPreferring:static_cast<uint32_t>(
                                                 std::max(standaloneHost->currentSampleRate, 0))];
    [self resetBufferSizeSelectionPreferring:standaloneHost->currentBufferSize];
    [self resetChannelSelectionsPreferringOutput:standaloneHost->deviceOutputFirstChannel
                                           input:standaloneHost->deviceInputFirstChannel];
    [self updateInputNote];
    [self applyAppearance];

    [content layoutSubtreeIfNeeded];
    [self setContentSize:[content fittingSize]];
  }
}

- (void)okButtonPressed:(id)sender
{
  @autoreleasepool
  {
    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();

    // every audio menu has opened what it shows already; not when the stream stopped since,
    // or a choice did not open. one that does not open now keeps the window up
    const auto shown = [self shownSetup];
    if (![self openShownAudio]) return;
    const auto &outId = shown.outputId;
    const auto &inId = shown.inputId;
    const bool useOut = shown.useOutput;
    const bool useIn = shown.useInput;
    const auto &outChannels = shown.outputChannels;
    const auto &inChannels = shown.inputChannels;
    const auto sr = shown.rate;

    // persist the choice, per plugin id - startAudioThread() opens what the file names
    // on the next launch, which otherwise falls back to the system default input, a
    // laptop's built-in microphone. only once it has opened, so a combination that
    // cannot run is never what a launch starts with; and what the user chose, not what was
    // opened, which may be a fallback (standalone_settings.h)
    auto &settings = standaloneHost->settings;
    standaloneHost->setChosenDevices(useOut ? outId : std::string{}, useIn ? inId : std::string{});
    settings.audioOutputUsed = useOut;
    settings.audioInputUsed = useIn;
    if (useOut) settings.outputFirstChannel = outChannels.first + 1;
    if (useIn) settings.inputFirstChannel = inChannels.first + 1;
    settings.sampleRate = (int32_t)sr;

    // a port list is applied only when it was changed, so that an untouched input list
    // keeps "every port" if that is what it had
    if (listChanged(midiInRows))
    {
      standaloneHost->setChosenMidiPorts(true, ticked(midiInRows));
      standaloneHost->openMidiPorts(ticked(midiInRows), false);
    }
    if (listChanged(midiOutRows))
    {
      standaloneHost->setChosenMidiPorts(false, ticked(midiOutRows));
      standaloneHost->openMidiOutputPorts(ticked(midiOutRows));
    }

    // likewise, so that an application connected to a virtual port keeps its connection
    const bool virtualIn = [virtualInCheck state] == NSControlStateValueOn;
    if (virtualIn != virtualInWasOn)
    {
      settings.midiVirtualInput = virtualIn;
      standaloneHost->setVirtualMidiInput(virtualIn);
    }
    const bool virtualOut = [virtualOutCheck state] == NSControlStateValueOn;
    if (virtualOut != virtualOutWasOn)
    {
      settings.midiVirtualOutput = virtualOut;
      standaloneHost->setVirtualMidiOutput(virtualOut);
    }

    // the window's own choice, never a plugin's (StandaloneHost::screenAppearanceFromPlugin)
    if (!standaloneHost->screenAppearanceFromPlugin)
      settings.screenAppearance = standaloneHost->screenAppearance;

    standaloneHost->captureAudioSettings();
    if (!standaloneHost->saveStandaloneSettings())
      LOGINFO("[WARNING] Unable to save the audio settings; the next launch will not have them");

    applied = true;

    [self close];
  }
}

- (void)defaultButtonPressed:(id)sender
{
  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  auto [systemIn, out, sr] = standaloneHost->getDefaultAudioInOutSampleRate();

  // what a first run opens: never a microphone, an interface in its place where there is
  // one, otherwise "No Input", and the note says why
  [self fillDeviceMenusSelectingOutput:out input:standaloneHost->defaultInputToOpen()];
  [self resetChannelSelectionsPreferringOutput:0 input:0];

  [self resetSampleRateSelectionPreferring:static_cast<uint32_t>(std::max(sr, 0))];
  [self resetBufferSizeSelectionPreferring:freeaudio::clap_wrapper::standalone::StandaloneSettings::
                                               defaultBufferSize];
  [self updateInputNote];
  [self updateBufferSizeTitles];
  [self openShownAudio];
}

- (void)close
{
  // once, whichever way the window goes - OK, Cancel or the title bar's close button,
  // which all end here. the plugin hears it on the next turn of the run loop, after the
  // window is gone and never from inside a call of its own
  // an appearance tried in the window and not confirmed with OK goes back
  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  if (!applied && !closeReported && standaloneHost->screenAppearance != appearanceWhenOpened)
    standaloneHost->setScreenAppearance(appearanceWhenOpened, false);

  // likewise the audio the menus opened: what ran when the window opened runs again. when
  // nothing ran, the saved settings are tried as a launch tries them, quietly
  if (!applied && !closeReported && audioOpenedHere)
  {
    if (audioWhenOpened.running)
    {
      openSetup(audioWhenOpened);
    }
    else
    {
      standaloneHost->applyAudioSettings();
      sa::StandaloneHost::SilenceAudioErrors silence(standaloneHost);
      standaloneHost->startAudioThreadOn(
          standaloneHost->audioInputDevice, standaloneHost->deviceInputChannels,
          standaloneHost->audioInputUsed && standaloneHost->numAudioInputs > 0,
          standaloneHost->audioOutputDevice, standaloneHost->deviceOutputChannels,
          standaloneHost->audioOutputUsed && standaloneHost->numAudioOutputs > 0,
          standaloneHost->currentSampleRate);
    }
  }

  if (!closeReported)
  {
    closeReported = true;
    if (openAudioSettings == self) openAudioSettings = nil;

    const uint32_t result = applied ? CLAP_WRAPPER_STANDALONE_CONFIGURATION_APPLIED
                                    : CLAP_WRAPPER_STANDALONE_CONFIGURATION_CANCELLED;
    dispatch_async(dispatch_get_main_queue(), ^{
      freeaudio::clap_wrapper::standalone::getStandaloneHost()->settingsScreenClosed(result);
    });
  }
  [super close];
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
  // the same channels on the other device, where it has them
  [self resetChannelSelectionsPreferringOutput:[self shownChoice:false].first
                                         input:[self shownChoice:true].first];
  [self resetSampleRateSelectionPreferring:static_cast<uint32_t>([[[sampleRateSelection selectedItem]
                                               title] integerValue])];
  [self resetBufferSizeSelectionPreferring:static_cast<uint32_t>(
                                               [[bufferSizeSelection selectedItem] tag])];
  [self updateInputNote];
  [self openShownAudio];
}

- (void)onSampleRateChanged:(id)sender
{
  [self updateBufferSizeTitles];
  [self openShownAudio];
}

- (void)onAudioMenuChanged:(id)sender
{
  [self openShownAudio];
}

// the stream the audio menus describe
- (AudioSetup)shownSetup
{
  AudioSetup setup;
  setup.running = true;
  const auto oidx = [outputSelection indexOfSelectedItem];
  if (oidx >= 0 && (size_t)oidx < outDevices.size())
  {
    setup.outputId = outDevices[oidx].id;
    setup.useOutput = true;
  }
  const auto iidx = [inputSelection indexOfSelectedItem];
  if (iidx > 0 && (size_t)iidx <= inDevices.size())
  {
    setup.inputId = inDevices[iidx - 1].id;
    setup.useInput = true;
  }
  setup.rate = static_cast<int32_t>([[[sampleRateSelection selectedItem] title] integerValue]);
  setup.frames = static_cast<uint32_t>([[bufferSizeSelection selectedItem] tag]);
  setup.outputChannels = [self shownChoice:false];
  setup.inputChannels = [self shownChoice:true];
  return setup;
}

// what the audio menus show, opened at once, so that a choice is heard and the latency
// beside the buffer size is measured. a choice that does not open has its error shown (by
// the host), the stream before it opened again, and the menus back on what runs. false
// then
- (bool)openShownAudio
{
  const auto shown = [self shownSetup];
  if (runsAs(shown))
  {
    [self updateLatencyNote];
    return true;
  }

  const auto before = runningSetup();
  if (openSetup(shown))
  {
    audioOpenedHere = true;
    // the size the device granted, where it is not the one asked for
    auto standaloneHost = sa::getStandaloneHost();
    if (shown.frames != standaloneHost->currentBufferSize)
      [self resetBufferSizeSelectionPreferring:standaloneHost->currentBufferSize];
    [self updateLatencyNote];
    return true;
  }

  if (before.running) openSetup(before);
  [self showRunningSetup];
  return false;
}

// the audio menus on the stream that runs, or on the host's devices when none does
- (void)showRunningSetup
{
  auto standaloneHost = sa::getStandaloneHost();
  [self fillDeviceMenusSelectingOutput:standaloneHost->audioOutputDevice
                                 input:standaloneHost->audioInputUsed ? standaloneHost->audioInputDevice
                                                                      : std::string{}];
  [self resetChannelSelectionsPreferringOutput:standaloneHost->deviceOutputFirstChannel
                                         input:standaloneHost->deviceInputFirstChannel];
  [self resetSampleRateSelectionPreferring:static_cast<uint32_t>(
                                               std::max(standaloneHost->currentSampleRate, 0))];
  [self resetBufferSizeSelectionPreferring:standaloneHost->currentBufferSize];
  [self updateInputNote];
}

// each size with how long it lasts at the selected rate, "256 (5.8 ms)". the item's tag
// holds the size, so the title is free to say more
- (void)updateBufferSizeTitles
{
  const auto rate = [[[sampleRateSelection selectedItem] title] integerValue];
  for (NSMenuItem *item in [bufferSizeSelection itemArray])
  {
    const auto frames = [item tag];
    [item setTitle:rate > 0 ? [NSString stringWithFormat:@"%ld (%.1f ms)", (long)frames,
                                                         1000.0 * (double)frames / (double)rate]
                            : [NSString stringWithFormat:@"%ld", (long)frames]];
  }
  [bufferSizeSelection synchronizeTitleAndSelectedItem];
  [self updateLatencyNote];
}

// the sizes both selected devices accept, from the standalone's list; selects the
// preferred size, or the largest below it, or the smallest there is
- (void)resetBufferSizeSelectionPreferring:(uint32_t)preferred
{
  namespace sa = freeaudio::clap_wrapper::standalone;
  auto standaloneHost = sa::getStandaloneHost();

  const auto oidx = [outputSelection indexOfSelectedItem];
  const auto iidx = [inputSelection indexOfSelectedItem];
  auto sizes = standaloneHost->getBufferSizesFor(
      oidx >= 0 && (size_t)oidx < outDevices.size() ? outDevices[oidx].id : std::string{},
      iidx > 0 && (size_t)iidx <= inDevices.size() ? inDevices[iidx - 1].id : std::string{});

  // the stream may run at a size the list does not have, which the device chose
  const auto running = standaloneHost->currentBufferSize;
  if (running > 0 && std::find(sizes.begin(), sizes.end(), running) == sizes.end())
  {
    sizes.insert(std::upper_bound(sizes.begin(), sizes.end(), running), running);
  }

  if (preferred == 0) preferred = sa::StandaloneSettings::defaultBufferSize;

  [bufferSizeSelection removeAllItems];
  NSInteger selected{sizes.empty() ? -1 : 0};
  for (size_t i = 0; i < sizes.size(); ++i)
  {
    [bufferSizeSelection addItemWithTitle:[NSString stringWithFormat:@"%u", sizes[i]]];
    [[bufferSizeSelection lastItem] setTag:sizes[i]];
    if (sizes[i] <= preferred) selected = (NSInteger)i;
  }
  if (selected >= 0) [bufferSizeSelection selectItemAtIndex:selected];
  [self updateBufferSizeTitles];
}

- (void)onAppearanceChanged:(id)sender
{
  // applied at once, through onScreenAppearanceChanged; saved by OK
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->setScreenAppearance(
      static_cast<uint32_t>([[appearanceSelection selectedItem] tag]), false);
}

- (void)applyAppearance
{
  auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
  applyScreenAppearance(self);
  [appearanceSelection selectItemWithTag:standaloneHost->screenAppearance];
  const bool hidden = !offerAppearance || standaloneHost->screenAppearanceFromPlugin;
  [appearanceRow setHidden:hidden];
  [appearanceRuleRow setHidden:hidden];
}

- (void)updateInputNote
{
  namespace sa = freeaudio::clap_wrapper::standalone;
  auto standaloneHost = sa::getStandaloneHost();

  NSString *text{nil};
  NSColor *color{[NSColor secondaryLabelColor]};

  auto idx = [inputSelection indexOfSelectedItem];
  if (idx > 0 && (size_t)idx <= inDevices.size() && inDevices[idx - 1].isBluetooth)
  {
    text = @"A Bluetooth microphone switches its device to telephone quality, for every "
           @"application, while it is open.";
    color = [NSColor systemOrangeColor];
  }
  else if (idx == 0 && standaloneHost->numAudioInputs > 0)
  {
    auto [in, out, sr] = standaloneHost->getDefaultAudioInOutSampleRate();
    const auto *device = standaloneHost->findDevice(in);
    if (device && device->isMicrophone)
      text = @"The default input is a microphone, which is opened only when chosen here.";
  }

  if (text)
  {
    [inputNote setStringValue:text];
    [inputNote setTextColor:color];
  }
  [inputNoteRow setHidden:text == nil];
}

// "Latency: in 5.3 ms, out 30.0 ms" for the running stream, while the window shows its
// devices, channels, rate and size: for anything else the numbers would describe another
// stream. not shown when they are not known
- (void)updateLatencyNote
{
  auto standaloneHost = sa::getStandaloneHost();
  const auto &stream = standaloneHost->currentStream;

  const auto oidx = [outputSelection indexOfSelectedItem];
  const auto iidx = [inputSelection indexOfSelectedItem];
  const std::string output{oidx >= 0 && (size_t)oidx < outDevices.size() ? outDevices[oidx].id
                                                                         : std::string{}};
  const std::string input{iidx > 0 && (size_t)iidx <= inDevices.size() ? inDevices[iidx - 1].id
                                                                       : std::string{}};
  const bool showsRunning{
      standaloneHost->isAudioRunning() && stream.sampleRate > 0 &&
      output == (standaloneHost->audioOutputUsed ? standaloneHost->audioOutputDevice : std::string{}) &&
      input == (standaloneHost->audioInputUsed ? standaloneHost->audioInputDevice : std::string{}) &&
      [[[sampleRateSelection selectedItem] title] integerValue] == (NSInteger)stream.sampleRate &&
      [[bufferSizeSelection selectedItem] tag] == (NSInteger)stream.bufferFrames &&
      [self shownChoice:false].first == standaloneHost->deviceOutputFirstChannel &&
      (input.empty() || [self shownChoice:true].first == standaloneHost->deviceInputFirstChannel)};

  NSString *text{nil};
  if (showsRunning)
  {
    auto ms = [&stream](uint32_t frames) { return 1000.0 * frames / stream.sampleRate; };
    const bool in{stream.inputChannels > 0 && stream.inputLatency > 0};
    const bool out{stream.outputChannels > 0 && stream.outputLatency > 0};
    if (in && out)
      text = [NSString stringWithFormat:@"Latency: in %.1f ms, out %.1f ms", ms(stream.inputLatency),
                                        ms(stream.outputLatency)];
    else if (out)
      text = [NSString stringWithFormat:@"Latency: out %.1f ms", ms(stream.outputLatency)];
    else if (in)
      text = [NSString stringWithFormat:@"Latency: in %.1f ms", ms(stream.inputLatency)];
  }

  // called from the polling timer too: the same text is left alone
  if (text && ![[latencyNote stringValue] isEqualToString:text]) [latencyNote setStringValue:text];
  [latencyNote setHidden:text == nil];
}

// the device menus from the host's list, with these selected; an output not there selects
// the first, an input not there "No Input"
- (void)fillDeviceMenusSelectingOutput:(const std::string &)output input:(const std::string &)input
{
  auto standaloneHost = sa::getStandaloneHost();

  outDevices = standaloneHost->getOutputAudioDevices();
  inDevices = standaloneHost->getInputAudioDevices();

  auto titles = [](const std::vector<sa::AudioDevice> &devices)
  {
    std::vector<const sa::AudioDevice *> pointers;
    for (const auto &d : devices) pointers.push_back(&d);
    return sa::deviceMenuTitles(pointers);
  };

  [outputSelection removeAllItems];
  const auto outTitles = titles(outDevices);
  for (size_t i = 0; i < outDevices.size(); ++i)
  {
    [outputSelection addItemWithTitle:[NSString stringWithUTF8String:outTitles[i].c_str()]];
    if (outDevices[i].id == output) [outputSelection selectItemAtIndex:(NSInteger)i];
  }

  [inputSelection removeAllItems];
  [inputSelection addItemWithTitle:@"No Input"];
  const auto inTitles = titles(inDevices);
  for (size_t i = 0; i < inDevices.size(); ++i)
  {
    [inputSelection addItemWithTitle:[NSString stringWithUTF8String:inTitles[i].c_str()]];
    if (!input.empty() && inDevices[i].id == input) [inputSelection selectItemAtIndex:(NSInteger)i + 1];
  }
}

// the channel menus for the selected devices, with the choice that starts at these
// channels selected, or the first; the input's row only while there is an input
- (void)resetChannelSelectionsPreferringOutput:(uint32_t)outputFirst input:(uint32_t)inputFirst
{
  auto standaloneHost = sa::getStandaloneHost();

  auto fill = [](NSPopUpButton *menu, std::vector<sa::AudioChannelChoice> &choices,
                 const std::vector<sa::AudioChannelChoice> &from, uint32_t first)
  {
    choices = from;
    [menu removeAllItems];
    for (size_t i = 0; i < choices.size(); ++i)
    {
      [menu addItemWithTitle:[NSString stringWithUTF8String:choices[i].title.c_str()]];
      if (choices[i].first == first) [menu selectItemAtIndex:(NSInteger)i];
    }
    [menu setEnabled:choices.size() > 1];
  };

  const auto oidx = [outputSelection indexOfSelectedItem];
  const auto iidx = [inputSelection indexOfSelectedItem];
  const bool hasOutput = oidx >= 0 && (size_t)oidx < outDevices.size();
  const bool hasInput = iidx > 0 && (size_t)iidx <= inDevices.size();

  fill(outputChannelSelection, outChoices,
       hasOutput ? standaloneHost->getChannelChoices(outDevices[oidx].id, false)
                 : std::vector<sa::AudioChannelChoice>{},
       outputFirst);
  fill(inputChannelSelection, inChoices,
       hasInput ? standaloneHost->getChannelChoices(inDevices[iidx - 1].id, true)
                : std::vector<sa::AudioChannelChoice>{},
       inputFirst);
  [inputChannelRow setHidden:!hasInput || standaloneHost->numAudioInputs == 0];
}

// the channels a menu shows; {0, 0} with none
- (sa::AudioChannelChoice)shownChoice:(bool)input
{
  const auto &choices = input ? inChoices : outChoices;
  const auto idx = [input ? inputChannelSelection : outputChannelSelection indexOfSelectedItem];
  if (idx < 0 || (size_t)idx >= choices.size()) return {};
  return choices[(size_t)idx];
}

// a device or a port came or went while the window is open: the menus and lists follow,
// and keep what is selected in them where it is still there
- (void)devicesChanged
{
  auto standaloneHost = sa::getStandaloneHost();

  const auto oidx = [outputSelection indexOfSelectedItem];
  const auto iidx = [inputSelection indexOfSelectedItem];
  const auto output =
      oidx >= 0 && (size_t)oidx < outDevices.size() ? outDevices[oidx].id : std::string{};
  const auto input =
      iidx > 0 && (size_t)iidx <= inDevices.size() ? inDevices[iidx - 1].id : std::string{};
  const auto rate = [[[sampleRateSelection selectedItem] title] integerValue];
  const auto frames = static_cast<uint32_t>([[bufferSizeSelection selectedItem] tag]);
  const auto outFirst = [self shownChoice:false].first;
  const auto inFirst = [self shownChoice:true].first;

  [self fillDeviceMenusSelectingOutput:output input:input];
  [self resetChannelSelectionsPreferringOutput:outFirst input:inFirst];
  [self resetSampleRateSelectionPreferring:static_cast<uint32_t>(std::max<NSInteger>(rate, 0))];
  [self resetBufferSizeSelectionPreferring:frames];
  [self updateInputNote];

  // rows already there keep what they show; a port plugged in now is ticked as the
  // settings would open it, and a saved one not plugged in stays listed
  std::vector<std::string> onIds, absentNames;
  standaloneHost->savedMidiSelection(true, onIds, absentNames);
  makePortRows(midiInRows, standaloneHost->getMidiInputPorts(), onIds, absentNames, midiInAllOn);
  layoutPortList(midiInList, midiInRows, @"No MIDI inputs found");
  standaloneHost->savedMidiSelection(false, onIds, absentNames);
  makePortRows(midiOutRows, standaloneHost->getMidiOutputPorts(), onIds, absentNames, false);
  layoutPortList(midiOutList, midiOutRows, @"No MIDI outputs found");

  NSView *content = [self contentView];
  [content layoutSubtreeIfNeeded];
  [self setContentSize:[content fittingSize]];
}

// the rates both selected devices run at, with this one selected where it is among them;
// otherwise 44100, else 48000, else the first (preferredSampleRate)
- (void)resetSampleRateSelectionPreferring:(uint32_t)rate
{
  [sampleRateSelection removeAllItems];

  auto idx = [outputSelection indexOfSelectedItem];
  if (idx < 0 || (size_t)idx >= outDevices.size())
  {
    // no output device at all: nothing to run a stream on
    [okButton setEnabled:NO];
    return;
  }
  const auto &oDev = outDevices[idx];

  idx = [inputSelection indexOfSelectedItem];
  const auto *iDev = idx > 0 && (size_t)idx <= inDevices.size() ? &inDevices[idx - 1] : nullptr;

  const auto rates = sa::commonSampleRates(&oDev, iDev);
  for (auto sr : rates)
  {
    [sampleRateSelection addItemWithTitle:[NSString stringWithFormat:@"%u", sr]];
  }
  if (const auto selected = sa::preferredSampleRate(rates, rate))
  {
    [sampleRateSelection selectItemWithTitle:[NSString stringWithFormat:@"%u", selected]];
  }

  // no rate in common, nothing to open the two devices at
  const bool canOpen = [sampleRateSelection numberOfItems] > 0;
  [okButton setEnabled:canOpen];
  [noRateRow setHidden:canOpen];
}

@end
