#include <iostream>

#include "detail/standalone/standalone_details.h"
#include "detail/standalone/entry.h"

#if LIN && CLAP_WRAPPER_STANDALONE_X11
#include "detail/standalone/linux/x11_gui.h"
#endif

#if LIN
#include "detail/standalone/linux/linux_frontend.h"
#include "detail/standalone/linux/linux_command_line.h"
#endif

// For now just a simple main. In the future this will branch out to
// an [NSApplicationMain ] and so on depending on platform
int main(int argc, char **argv)
{
#if LIN
  // Before anything else, so that a ^C during startup still unwinds through
  // shutdown rather than dropping the process where it stands
  freeaudio::clap_wrapper::standalone::linux_standalone::installSignalHandlers();

  freeaudio::clap_wrapper::standalone::linux_standalone::CommandLineOptions clOptions;
  {
    using namespace freeaudio::clap_wrapper::standalone::linux_standalone;
    switch (parseCommandLine(argc, argv, OUTPUT_NAME, clOptions))
    {
      case CommandLineResult::exitOk:
        return 0;
      case CommandLineResult::exitError:
        return 2;
      case CommandLineResult::run:
        break;
    }
  }
#endif

  auto fatalError = [](const std::string &msg)
  {
#if LIN
    freeaudio::clap_wrapper::standalone::linux_standalone::reportError("Unable to start", msg);
#else
    std::cerr << "Clap Standalone: " << msg << std::endl;
#endif
  };

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

  // A false here means we have no display, or --no-gui was passed. That is not
  // fatal: audio, MIDI and plugin timers all still run, we just never show a
  // window.
  x11Gui.initialize(freeaudio::clap_wrapper::standalone::getStandaloneHost(), !clOptions.noGui);
#endif

  if (!entry)
  {
    fatalError("No CLAP entry as configured. Is the plugin installed?");
    return 3;
  }

#if LIN
  // stderr always, plus a zenity/kdialog box when the session has one
  freeaudio::clap_wrapper::standalone::linux_standalone::installAudioErrorReporter();

  // Before any audio starts: RtAudio would otherwise settle on raw ALSA even on
  // a PipeWire box, because ALSA always has devices and it probes that first
  freeaudio::clap_wrapper::standalone::linux_standalone::selectAudioApi(clOptions.audioApi);

  // A device or rate the user asked for and which doesn't exist is a startup
  // error, not something to quietly substitute a default for
  if (!freeaudio::clap_wrapper::standalone::linux_standalone::applyCommandLineOptions(clOptions))
  {
    return 5;
  }
#endif

  std::string pid{PLUGIN_ID};
  int pindex{PLUGIN_INDEX};

  auto plugin =
      freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, (char **)argv);
  if (!plugin)
  {
    // Everything downstream of here dereferences this, so stop now rather than
    // crashing in the GUI handshake
    fatalError("Unable to create the plugin" + (pid.empty() ? std::string() : " '" + pid + "'") +
               ". See the log for details.");
    freeaudio::clap_wrapper::standalone::mainFinish();
    return 4;
  }

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
