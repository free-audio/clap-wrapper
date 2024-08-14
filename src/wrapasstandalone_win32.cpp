#include "detail/standalone/standalone_details.h"
#include "detail/standalone/entry.h"
#include "detail/standalone/windows/host_window.h"
#include "detail/standalone/windows/helpers.h"

int main(int argc, char** argv)
{
  const clap_plugin_entry* entry{nullptr};
#ifdef STATICALLY_LINKED_CLAP_ENTRY
  extern const clap_plugin_entry clap_entry;
  entry = &clap_entry;
#else
  std::string clapName{HOSTED_CLAP_NAME};
  freeaudio::clap_wrapper::standalone::windows::helpers::log("Loading {}", clapName);

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
    freeaudio::clap_wrapper::standalone::windows::helpers::errorBox("No entry as configured");
    freeaudio::clap_wrapper::standalone::windows::helpers::abort(3);
  }

  auto clapPlugin{
      freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, PLUGIN_ID, PLUGIN_INDEX, argc, argv)};
  freeaudio::clap_wrapper::standalone::mainStartAudio();

  if (clapPlugin->_ext._gui->is_api_supported(clapPlugin->_plugin, CLAP_WINDOW_API_WIN32, false))
  {
    freeaudio::clap_wrapper::standalone::windows::HostWindow hostWindow{clapPlugin};

    return freeaudio::clap_wrapper::standalone::windows::helpers::messageLoop();
  }
  else
  {
    freeaudio::clap_wrapper::standalone::windows::helpers::errorBox(
        "CLAP_WINDOW_API_WIN32 is not supported");
    freeaudio::clap_wrapper::standalone::windows::helpers::abort();
  }
}
