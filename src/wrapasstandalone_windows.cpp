#include <algorithm>
#include <cstdio>

#include "detail/standalone/windows/windows_standalone.h"

int WINAPI wWinMain(HINSTANCE /* hInstance */, HINSTANCE /* hPrevInstance */, PWSTR /* pCmdLine */,
                    int nCmdShow)
{
  auto coUninitialize{wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};

  {
    // a windows application has no console of its own: the listing goes to the one it was
    // started from
    auto args{freeaudio::clap_wrapper::standalone::windows_standalone::getArgs()};
    std::vector<const char *> argv;
    for (const auto &arg : args) argv.push_back(arg.c_str());
    if (std::find(args.begin(), args.end(), "--list-audio-devices") != args.end() &&
        ::AttachConsole(ATTACH_PARENT_PROCESS))
    {
      FILE *console{nullptr};
      freopen_s(&console, "CONOUT$", "w", stdout);
    }
    if (freeaudio::clap_wrapper::standalone::mainListAudioDevices(static_cast<int>(argv.size()),
                                                                  argv.data()))
      return 0;
  }

  const clap_plugin_entry *entry{nullptr};
#ifdef STATICALLY_LINKED_CLAP_ENTRY
  extern const clap_plugin_entry clap_entry;
  entry = &clap_entry;
#else
  std::string clapName{HOSTED_CLAP_NAME};
  freeaudio::clap_wrapper::standalone::windows_standalone::log("Loading {}", clapName);

  auto lib{Clap::Library()};

  for (const auto &searchPaths : Clap::getValidCLAPSearchPaths())
  {
    auto clapPath{searchPaths / (clapName + ".clap")};

    if (fs::exists(clapPath) && !entry)
    {
      lib.load(clapPath);
      entry = lib._pluginEntry;
    }
  }
#endif

  if (!entry)
  {
    freeaudio::clap_wrapper::standalone::windows_standalone::log("No entry as configured");

    return 3;
  }

  auto args{freeaudio::clap_wrapper::standalone::windows_standalone::getArgs()};

  std::vector<char *> argv;

  for (auto i = 0; i < args.size(); i++)
  {
    argv.emplace_back(args[i].data());
  }

  // Before the plugin is created, so that supports_audio_midi_configuration_screen
  // answers true from init() on. Showing works once the plugin window exists.
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onShowSettingsScreen =
      &freeaudio::clap_wrapper::standalone::windows_standalone::showSettingsScreen;

  auto clapPlugin{freeaudio::clap_wrapper::standalone::mainCreatePlugin(
      entry, PLUGIN_ID, PLUGIN_INDEX, static_cast<int>(argv.size()), argv.data())};

  if (!clapPlugin)
  {
    // The Plugin constructor dereferences this immediately, so a failed create
    // used to be a null dereference rather than a message.
    freeaudio::clap_wrapper::standalone::windows_standalone::MessageHandler{}.error(
        "Unable to create the plugin. The CLAP was found but did not provide '" PLUGIN_ID "'.");

    return 4;
  }

  {
    freeaudio::clap_wrapper::standalone::windows_standalone::Plugin plugin{clapPlugin, nCmdShow};

    // Drop our own reference before the scope ends. The wrapper's shutdown runs
    // from WM_DESTROY inside run(), and the CLAP must not outlive it.
    clapPlugin.reset();

    return freeaudio::clap_wrapper::standalone::windows_standalone::run();
  }
}
