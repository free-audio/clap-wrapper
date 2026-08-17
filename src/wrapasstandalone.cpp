#include <iostream>

#include "detail/standalone/standalone_details.h"
#include "detail/standalone/entry.h"

#if LIN && CLAP_WRAPPER_STANDALONE_X11
#include "detail/standalone/linux/x11_gui.h"
#endif

#if LIN
#include "detail/standalone/linux/linux_frontend.h"
#endif

// For now just a simple main. In the future this will branch out to
// an [NSApplicationMain ] and so on depending on platform
int main(int argc, char **argv)
{
#if LIN
  // Before anything else, so that a ^C during startup still unwinds through
  // shutdown rather than dropping the process where it stands
  freeaudio::clap_wrapper::standalone::linux_standalone::installSignalHandlers();
#endif

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

#if LIN && CLAP_WRAPPER_STANDALONE_X11
  freeaudio::clap_wrapper::standalone::linux_standalone::X11Gui x11Gui{};

  x11Gui.initialize(freeaudio::clap_wrapper::standalone::getStandaloneHost());
#endif

  if (!entry)
  {
    std::cerr << "Clap Standalone: No Entry as configured" << std::endl;
    return 3;
  }

#if LIN
  // Without this every RtAudio failure is silent and the app just runs with no
  // sound. reportError writes stderr always, and puts up a zenity/kdialog box
  // if the desktop has one.
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->displayAudioError =
      [](const std::string &msg)
  {
    freeaudio::clap_wrapper::standalone::linux_standalone::reportError("Unable to configure audio", msg);
  };
#endif

  std::string pid{PLUGIN_ID};
  int pindex{PLUGIN_INDEX};

  auto plugin =
      freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, (char **)argv);
  freeaudio::clap_wrapper::standalone::mainStartAudio();

#if LIN && CLAP_WRAPPER_STANDALONE_X11
  x11Gui.setPlugin(plugin);
  x11Gui.runloop();
  x11Gui.shutdown();
#elif LIN
  // No GUI compiled in, so idle here until the host winds down or a signal
  // arrives. mainWait() would not notice the signal.
  freeaudio::clap_wrapper::standalone::linux_standalone::waitForQuit();
#else
  freeaudio::clap_wrapper::standalone::mainWait();
#endif

  plugin = nullptr;
  freeaudio::clap_wrapper::standalone::mainFinish();
}
