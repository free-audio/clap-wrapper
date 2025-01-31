#include <iostream>

#include "detail/standalone/standalone_details.h"
#include "detail/standalone/entry.h"

#if LIN
#if CLAP_WRAPPER_HAS_GTK3
#include "detail/standalone/linux/gtkutils.h"
#endif
#endif

// For now just a simple main. In the future this will branch out to
// an [NSApplicationMain ] and so on depending on platform
int main(int argc, char **argv)
{
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

    if (fs::exists(clapPath) && !entry)
    {
      lib.load(clapPath);
      entry = lib._pluginEntry;
    }
  }

#endif

#if LIN
#if CLAP_WRAPPER_HAS_GTK3
  freeaudio::clap_wrapper::standalone::linux_standalone::GtkGui gtkGui{};

  if (!gtkGui.parseCommandLine(argc, argv))
  {
    return 1;
  }
  gtkGui.initialize(freeaudio::clap_wrapper::standalone::getStandaloneHost());
#endif
#endif

  if (!entry)
  {
    std::cerr << "Clap Standalone: No Entry as configured" << std::endl;
    return 3;
  }

  std::string pid{PLUGIN_ID};
  int pindex{PLUGIN_INDEX};

  auto plugin =
      freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, (char **)argv);
  freeaudio::clap_wrapper::standalone::mainStartAudio();

#if LIN
#if CLAP_WRAPPER_HAS_GTK3
  gtkGui.setPlugin(plugin);
  gtkGui.runloop(argc, argv);
  gtkGui.shutdown();
  gtkGui.setPlugin(nullptr);
#else
  freeaudio::clap_wrapper::standalone::mainWait();
#endif
#else
  freeaudio::clap_wrapper::standalone::mainWait();
#endif

  plugin = nullptr;
  freeaudio::clap_wrapper::standalone::mainFinish();
}
