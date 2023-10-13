#import "AppDelegate.h"

#include <AVFoundation/AVFoundation.h>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"

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

  auto pluginProxy = plugin->getProxy();
  if (pluginProxy->canUseGui())
  {
    if (!pluginProxy->guiIsApiSupported(CLAP_WINDOW_API_COCOA, false)) LOG << "NO COCOA " << std::endl;

    pluginProxy->guiCreate(CLAP_WINDOW_API_COCOA, false);
    pluginProxy->guiSetScale(1);

    uint32_t w, h;
    pluginProxy->guiGetSize(&w, &h);

    NSView *view = [[self window] contentView];

    NSSize sz;
    sz.width = w;
    sz.height = h;
    [[self window] setContentSize:sz];

    clap_window win;
    win.api = CLAP_WINDOW_API_COCOA;
    win.cocoa = view;
    pluginProxy->guiSetParent(&win);
    pluginProxy->guiShow();
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

  if (plugin && plugin->getProxy()->canUseGui())
  {
    plugin->getProxy()->guiHide();
    plugin->getProxy()->guiDestroy();
  }

  // Insert code here to tear down your application
  freeaudio::clap_wrapper::standalone::mainFinish();
}

@end
