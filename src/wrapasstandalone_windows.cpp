#include "detail/standalone/windows/windows_standalone.h"

int wWinMain(::HINSTANCE, ::HINSTANCE, wchar_t*, int)
{
  auto coUninitialize{wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};

  const clap_plugin_entry* entry{nullptr};
#ifdef STATICALLY_LINKED_CLAP_ENTRY
  extern const clap_plugin_entry clap_entry;
  entry = &clap_entry;
#else
  std::string clapName{HOSTED_CLAP_NAME};
  freeaudio::clap_wrapper::standalone::windows_standalone::log("Loading {}", clapName);

  auto lib{Clap::Library()};

  for (const auto& searchPaths : Clap::getValidCLAPSearchPaths())
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

  std::vector<char*> argv;

  for (int i = 0; i < args.first; i++)
  {
    argv.emplace_back(args.second[i].data());
  }

  auto clapPlugin{freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, PLUGIN_ID, PLUGIN_INDEX,
                                                                        args.first, argv.data())};

  freeaudio::clap_wrapper::standalone::windows_standalone::Plugin plugin{clapPlugin};

  return freeaudio::clap_wrapper::standalone::windows_standalone::run();
}
