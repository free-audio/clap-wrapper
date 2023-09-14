#import "AppDelegate.h"

#include <AVFoundation/AVFoundation.h>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"

#include "detail/clap/fsutil.h"

@interface AppDelegate ()

@property(assign) IBOutlet NSWindow *window;
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
      lib.load(clapPath.u8string().c_str());
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

  auto plugin = Clap::Standalone::mainCreatePlugin(entry, pid, pindex, 1, (char **)argv);

  [[self window] orderFrontRegardless];

  if (plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto p = plugin->_plugin;
    if (!ui->is_api_supported(p, CLAP_WINDOW_API_COCOA, false)) LOG << "NO COCOA " << std::endl;

    ui->create(p, CLAP_WINDOW_API_COCOA, false);
    ui->set_scale(p, 1);

    uint32_t w, h;
    ui->get_size(p, &w, &h);

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

  Clap::Standalone::mainStartAudio();
}

- (void)applicationWillTerminate:(NSNotification *)aNotification
{
  LOG << "applicationWillTerminate shutdown" << std::endl;
  auto plugin = Clap::Standalone::getMainPlugin();

  if (plugin && plugin->_ext._gui)
  {
    plugin->_ext._gui->hide(plugin->_plugin);
    plugin->_ext._gui->destroy(plugin->_plugin);
  }

  // Insert code here to tear down your application
  Clap::Standalone::mainFinish();
}

@end
