#import "AppDelegate.h"

#include <AVFoundation/AVFoundation.h>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"

#include "detail/clap/fsutil.h"

@interface AppDelegate ()

@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
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
  LOG << "Loading " << clapName << std::endl;

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

  if (plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto p = plugin->_plugin;
    if (!ui->is_api_supported(p, CLAP_WINDOW_API_COCOA, false)) LOG << "NO COCOA " << std::endl;

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

  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize =
      [self](uint32_t w, uint32_t h)
  {
    NSSize sz;
    sz.width = w;
    sz.height = h;
    [[self window] setContentSize:sz];
    return false;
  };
  freeaudio::clap_wrapper::standalone::mainStartAudio();
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  LOG << "applicationWillTerminate shutdown" << std::endl;
  auto plugin = freeaudio::clap_wrapper::standalone::getMainPlugin();

  if (plugin && plugin->_ext._gui)
  {
    plugin->_ext._gui->hide(plugin->_plugin);
    plugin->_ext._gui->destroy(plugin->_plugin);
  }

  // Insert code here to tear down your application
  freeaudio::clap_wrapper::standalone::mainFinish();
}

- (IBAction)openAudioSettingsWindow:(id)sender
{
  NSLog(@"openAudioSettingsWindow: Unimplemented");
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

- (IBAction)saveDocumentAs:(id)sender
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

- (IBAction)openDocument:(id)sender
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
