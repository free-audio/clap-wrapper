#import "AppDelegate.h"

#include <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"

#include "detail/clap/fsutil.h"

@interface ClapWrapperAppDelegate ()

@end

namespace
{
// a checkbox in one of the settings window's midi port lists
struct MidiPortRow
{
  std::string name;
  NSButton *check;  // owned by the window's views
  bool wasOn;       // when the window opened
};
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
  std::vector<RtAudio::DeviceInfo> outDevices, inDevices;

  // a list nobody touched leaves its settings as they were, which for the inputs may be
  // "every port", including any plugged in later
  std::vector<MidiPortRow> midiInRows, midiOutRows;

  // the optional virtual ports, and whether each was asked for when the window opened
  NSButton *virtualInCheck, *virtualOutCheck;
  bool virtualInWasOn, virtualOutWasOn;

  // OK reopened audio with what the window shows; Cancel and the close button did not
  bool applied;
  bool closeReported;

  // OK is off, and the note says why, while the input and output have no sample rate in
  // common: one stream runs both at one rate, so the devices could not be opened together
  NSButton *okButton;
  NSGridRow *noRateRow;

  // under the input menu, while it matters: that a bluetooth microphone takes its whole
  // device down to telephone quality, or that the default input is a microphone and was not
  // opened
  NSTextField *inputNote;
  NSGridRow *inputNoteRow;
}

- (void)setupContents;
- (void)resetSampleRateSelection;
- (void)resetBufferSizeSelectionPreferring:(uint32_t)frames;
- (void)updateBufferSizeTitles;
- (void)updateInputNote;

@end

// the settings window while it is open. not retained: it is released when it closes
// (isReleasedWhenClosed), and clears this before that
static AudioSettingsWindow *openAudioSettings{nil};

@implementation ClapWrapperAppDelegate

- (void)timerCallback:(NSTimer *)instance
{
  auto *standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
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
        [alert runModal];
      }
      return;
    }
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
// that unticking one is a way to forget it; a plugin that could not use the ports gets
// them listed and greyed
NSScrollView *makePortList(std::vector<MidiPortRow> &rows, const std::vector<std::string> &available,
                           const std::vector<std::string> &on, bool allOn, bool usable,
                           NSString *emptyText)
{
  rows.clear();
  auto addRow = [&rows, usable](const std::string &name, bool connected, bool isOn)
  {
    NSString *title = [NSString stringWithUTF8String:name.c_str()];
    if (!connected) title = [title stringByAppendingString:@" (not connected)"];
    NSButton *check = [NSButton checkboxWithTitle:title target:nil action:nil];
    [check setState:isOn ? NSControlStateValueOn : NSControlStateValueOff];
    [check setEnabled:usable];
    [[check cell] setLineBreakMode:NSLineBreakByTruncatingTail];
    rows.push_back({name, check, isOn});
  };
  for (const auto &name : available) addRow(name, true, allOn || contains(on, name));
  if (!allOn)
    for (const auto &name : on)
      if (!contains(available, name)) addRow(name, false, true);

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

  NSScrollView *list =
      [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, frame.width, frame.height)] autorelease];
  [list setBorderType:NSBezelBorder];
  [list setHasVerticalScroller:scrolls];
  [list setAutohidesScrollers:YES];
  [list setDrawsBackground:YES];
  [list setBackgroundColor:[NSColor controlBackgroundColor]];
  [list setDocumentView:doc];
  [list setTranslatesAutoresizingMaskIntoConstraints:NO];
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
  std::vector<std::string> names;
  for (const auto &row : rows)
    if ([row.check state] == NSControlStateValueOn) names.push_back(row.name);
  return names;
}
}  // namespace

@implementation AudioSettingsWindow

- (void)setupContents
{
  @autoreleasepool
  {
    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();
    NSString *pluginName = pluginDisplayName();

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
    [sampleRateSelection setAction:@selector(updateBufferSizeTitles)];
    [sampleRateSelection setTarget:self];

    outDevices = standaloneHost->getOutputAudioDevices();
    // add items to menu
    int selIdx{-1}, idx{0};
    //[outputSelection addItemWithTitle:@"No Output"];

    for (auto o : outDevices)
    {
      [outputSelection addItemWithTitle:[NSString stringWithUTF8String:o.name.c_str()]];
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
      [inputSelection addItemWithTitle:[NSString stringWithUTF8String:i.name.c_str()]];
      if (standaloneHost->audioInputDeviceID == i.ID) selIdx = idx;
      idx++;
    }
    // the id is the default input's even with no input open, and showing that device
    // would have OK open it
    if (selIdx >= 0 && standaloneHost->audioInputUsed)
    {
      [inputSelection selectItemAtIndex:selIdx];
    }

    [inputSelection setAction:@selector(onSourceMenuChanged:)];
    [inputSelection setTarget:self];

    NSButton *defaultButton = [NSButton buttonWithTitle:@"Reset to System Default"
                                                 target:self
                                                 action:@selector(defaultButtonPressed:)];

    NSTextField *noRateNote =
        makeNote(@"These devices have no sample rate in common.", [NSColor systemRedColor]);
    inputNote = makeNote(@"", [NSColor secondaryLabelColor]);

    // midi
    const auto &settings = standaloneHost->settings;
    const bool takesMidi = standaloneHost->hasMIDIInput || standaloneHost->hasClapNoteInput;
    const bool sendsMidi = standaloneHost->createsMidiOutput;

    NSScrollView *midiInList =
        makePortList(midiInRows, standaloneHost->getMidiPortNames(), settings.midiPortNames,
                     settings.midiBindAllPorts, takesMidi, @"No MIDI inputs found");
    NSScrollView *midiOutList =
        makePortList(midiOutRows, standaloneHost->getMidiOutputPortNames(), settings.midiOutputPortNames,
                     false, sendsMidi, @"No MIDI outputs found");
    NSTextField *midiInNote = makeNote([NSString stringWithFormat:@"%@ takes no MIDI.", pluginName],
                                       [NSColor secondaryLabelColor]);
    NSTextField *midiOutNote = makeNote([NSString stringWithFormat:@"%@ sends no MIDI.", pluginName],
                                        [NSColor secondaryLabelColor]);

    // offered where the midi api has virtual ports, which on macos is every version
    const bool virtualPorts =
        freeaudio::clap_wrapper::standalone::StandaloneHost::supportsVirtualMidiPorts();
    auto makeVirtualCheck = [&](bool input, bool isOn, bool usable)
    {
      NSString *name =
          [NSString stringWithUTF8String:standaloneHost->virtualMidiPortName(input).c_str()];
      NSString *title = [NSString
          stringWithFormat:input ? @"Virtual input \u201C%@\u201D" : @"Virtual output \u201C%@\u201D",
                           name];
      NSButton *check = [NSButton checkboxWithTitle:title target:nil action:nil];
      [check setState:isOn ? NSControlStateValueOn : NSControlStateValueOff];
      [check setEnabled:usable];
      [check setToolTip:input ? @"A MIDI destination other applications can send to"
                              : @"A MIDI source other applications can receive from"];
      return check;
    };
    virtualInWasOn = settings.midiVirtualInput;
    virtualOutWasOn = settings.midiVirtualOutput;
    virtualInCheck = makeVirtualCheck(true, virtualInWasOn, takesMidi);
    virtualOutCheck = makeVirtualCheck(false, virtualOutWasOn, sendsMidi);

    NSTextField *midiInLabel = makeLabel(@"MIDI Inputs:");
    NSTextField *midiOutLabel = makeLabel(@"MIDI Outputs:");
    NSView *none = [NSGridCell emptyContentView];
    NSBox *sectionRule = makeRule();

    NSGridView *grid = [NSGridView gridViewWithViews:@[
      @[ makeLabel(@"Output:"), outputSelection ],
      @[ makeLabel(@"Input:"), inputSelection ],
      @[ none, inputNote ],
      @[ makeLabel(@"Sample Rate:"), sampleRateSelection ],
      @[ makeLabel(@"Buffer Size:"), bufferSizeSelection ],
      @[ none, defaultButton ],
      @[ none, noRateNote ],
      @[ sectionRule, none ],
      @[ midiInLabel, midiInList ],
      @[ none, virtualInCheck ],
      @[ none, midiInNote ],
      @[ midiOutLabel, midiOutList ],
      @[ none, virtualOutCheck ],
      @[ none, midiOutNote ],
    ]];
    [grid setRowSpacing:8];
    [grid setColumnSpacing:8];
    [grid setRowAlignment:NSGridRowAlignmentFirstBaseline];
    [[grid columnAtIndex:0] setXPlacement:NSGridCellPlacementTrailing];

    auto rowOf = [grid](NSView *v) { return [[grid cellForView:v] row]; };

    noRateRow = rowOf(noRateNote);
    [noRateRow setHidden:YES];
    inputNoteRow = rowOf(inputNote);
    [inputNoteRow setHidden:YES];
    [rowOf(midiInNote) setHidden:takesMidi];
    [rowOf(midiOutNote) setHidden:sendsMidi];
    [rowOf(virtualInCheck) setHidden:!virtualPorts];
    [rowOf(virtualOutCheck) setHidden:!virtualPorts];

    NSGridRow *ruleRow = rowOf(sectionRule);
    [ruleRow mergeCellsInRange:NSMakeRange(0, 2)];
    [ruleRow setRowAlignment:NSGridRowAlignmentNone];
    [ruleRow setYPlacement:NSGridCellPlacementCenter];
    [ruleRow setTopPadding:6];
    [ruleRow setBottomPadding:6];
    [[ruleRow cellAtIndex:0] setXPlacement:NSGridCellPlacementFill];

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

    [self resetSampleRateSelection];
    [self resetBufferSizeSelectionPreferring:standaloneHost->currentBufferSize];
    [self updateInputNote];

    [content layoutSubtreeIfNeeded];
    [self setContentSize:[content fittingSize]];
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
    const auto frames = static_cast<uint32_t>([[bufferSizeSelection selectedItem] tag]);

    auto standaloneHost = freeaudio::clap_wrapper::standalone::getStandaloneHost();

    // the stream running now, to go back to if the new one cannot be opened
    const bool wasRunning = standaloneHost->isAudioRunning();
    const auto previousIn = standaloneHost->audioInputDeviceID;
    const auto previousOut = standaloneHost->audioOutputDeviceID;
    const bool previousUseIn = standaloneHost->audioInputUsed;
    const bool previousUseOut = standaloneHost->audioOutputUsed;
    const auto previousRate = standaloneHost->currentSampleRate;
    const auto previousFrames = standaloneHost->currentBufferSize;

    // a stream already running on what the window shows is left alone: reopening it, for
    // a window in which only the midi ports changed, would drop out the audio for nothing
    const bool audioAsShown =
        wasRunning && (int32_t)sr == previousRate && (frames == 0 || frames == previousFrames) &&
        useOut == (standaloneHost->currentOutputChannels > 0) && (!useOut || outId == previousOut) &&
        useIn == (standaloneHost->currentInputChannels > 0) && (!useIn || inId == previousIn);

    if (!audioAsShown)
    {
      // the size to ask for; the stream reads back what the device granted
      if (frames > 0) standaloneHost->currentBufferSize = frames;
      standaloneHost->startAudioThreadOn(inId, 2, useIn, outId, 2, useOut, (int32_t)sr);
      if (!standaloneHost->isAudioRunning())
      {
        // neither kept nor saved, and the window stays up for another choice: the
        // devices are put back as they were, and the error callback has already told
        // the user why these did not open
        standaloneHost->currentBufferSize = previousFrames;
        if (wasRunning)
          standaloneHost->startAudioThreadOn(previousIn, 2, previousUseIn, previousOut, 2,
                                             previousUseOut, previousRate);
        return;
      }
    }

    // persist the choice, per plugin id - startAudioThread() opens what the file names
    // on the next launch, which otherwise falls back to the system default input, a
    // laptop's built-in microphone. only once it has opened, so a combination that
    // cannot run is never what a launch starts with; and by name, what the user chose,
    // not what was opened, which may be a fallback (standalone_settings.h)
    auto &settings = standaloneHost->settings;
    if (useOut) settings.outputDeviceName = outDevices[oidx].name;
    settings.audioOutputUsed = useOut;
    if (useIn) settings.inputDeviceName = inDevices[iidx - 1].name;
    settings.audioInputUsed = useIn;
    settings.sampleRate = (int32_t)sr;

    // a port list is applied only when it was changed, so that an untouched input list
    // keeps "every port" if that is what it had
    if (listChanged(midiInRows))
    {
      settings.midiBindAllPorts = false;
      settings.midiPortNames = ticked(midiInRows);
      standaloneHost->openMidiPorts(settings.midiPortNames, false);
    }
    if (listChanged(midiOutRows))
    {
      settings.midiOutputPortNames = ticked(midiOutRows);
      standaloneHost->openMidiOutputPorts(settings.midiOutputPortNames);
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
  auto [in, out, sr] = standaloneHost->getDefaultAudioInOutSampleRate();

  // a default never opens a microphone (isMicrophoneAudioDevice), so neither does this:
  // "No Input", and the note says why
  [inputSelection selectItemAtIndex:0];
  int idx = 1;
  if (!freeaudio::clap_wrapper::standalone::isMicrophoneAudioDevice(standaloneHost->deviceName(in)))
  {
    for (auto i : inDevices)
    {
      if ((int)i.ID == (int)in)
      {
        [inputSelection selectItemAtIndex:idx];
      }
      idx++;
    }
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
  [self resetBufferSizeSelectionPreferring:freeaudio::clap_wrapper::standalone::StandaloneSettings::
                                               defaultBufferSize];
  [self updateInputNote];

  for (NSMenuItem *item in [sampleRateSelection itemArray])
  {
    const auto sri = [[item title] integerValue];
    if ((int)sr == (int)sri)
    {
      [sampleRateSelection selectItem:item];
    }
  }
  [self updateBufferSizeTitles];
}

- (void)close
{
  // once, whichever way the window goes - OK, Cancel or the title bar's close button,
  // which all end here. the plugin hears it on the next turn of the run loop, after the
  // window is gone and never from inside a call of its own
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
  [self resetSampleRateSelection];
  [self resetBufferSizeSelectionPreferring:static_cast<uint32_t>(
                                               [[bufferSizeSelection selectedItem] tag])];
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
}

// the sizes both selected devices accept, from the standalone's list; selects the
// preferred size, or the largest below it, or the smallest there is
- (void)resetBufferSizeSelectionPreferring:(uint32_t)preferred
{
  namespace sa = freeaudio::clap_wrapper::standalone;
  auto standaloneHost = sa::getStandaloneHost();

  // rtaudio clamps a size outside a device's range without a word, so the menu does not
  // offer one. a device that cannot tell its range narrows nothing
  uint32_t lowest{0}, highest{std::numeric_limits<uint32_t>::max()};
  auto narrowTo = [&](const std::string &name)
  {
    const auto [min, max] = sa::bufferFrameSizeRange(name);
    if (max == 0) return;
    lowest = std::max(lowest, min);
    highest = std::min(highest, max);
  };
  const auto oidx = [outputSelection indexOfSelectedItem];
  if (oidx >= 0 && (size_t)oidx < outDevices.size()) narrowTo(outDevices[oidx].name);
  const auto iidx = [inputSelection indexOfSelectedItem];
  if (iidx > 0 && (size_t)iidx <= inDevices.size()) narrowTo(inDevices[iidx - 1].name);

  std::vector<uint32_t> sizes;
  for (auto size : standaloneHost->getBufferSizes())
  {
    if (size >= lowest && size <= highest) sizes.push_back(size);
  }

  // the stream may run at a size the list does not have, which the device chose
  const auto running = standaloneHost->currentBufferSize;
  if (running >= lowest && running <= highest && running > 0 &&
      std::find(sizes.begin(), sizes.end(), running) == sizes.end())
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

- (void)updateInputNote
{
  namespace sa = freeaudio::clap_wrapper::standalone;
  auto standaloneHost = sa::getStandaloneHost();

  NSString *text{nil};
  NSColor *color{[NSColor secondaryLabelColor]};

  auto idx = [inputSelection indexOfSelectedItem];
  if (idx > 0 && (size_t)idx <= inDevices.size() && sa::isBluetoothAudioDevice(inDevices[idx - 1].name))
  {
    text = @"A Bluetooth microphone switches its device to telephone quality, for every "
           @"application, while it is open.";
    color = [NSColor systemOrangeColor];
  }
  else if (idx == 0 && standaloneHost->numAudioInputs > 0)
  {
    auto [in, out, sr] = standaloneHost->getDefaultAudioInOutSampleRate();
    if (sa::isMicrophoneAudioDevice(standaloneHost->deviceName(in)))
      text = @"The default input is a microphone, which is opened only when chosen here.";
  }

  if (text)
  {
    [inputNote setStringValue:text];
    [inputNote setTextColor:color];
  }
  [inputNoteRow setHidden:text == nil];
}

- (void)resetSampleRateSelection
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

  // no rate in common, nothing to open the two devices at
  const bool canOpen = [sampleRateSelection numberOfItems] > 0;
  [okButton setEnabled:canOpen];
  [noRateRow setHidden:canOpen];
}

@end
