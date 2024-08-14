#include <memory>

#include "standalone_details.h"
#include "standalone_host.h"
#include "entry.h"

namespace freeaudio::clap_wrapper::standalone
{

static std::unique_ptr<StandaloneHost> standaloneHost;
std::shared_ptr<Clap::Plugin> plugin;
const clap_plugin_entry *entry{nullptr};

std::shared_ptr<Clap::Plugin> mainCreatePlugin(const clap_plugin_entry *ee, const std::string &clapId,
                                               uint32_t clapIndex, int argc, char **argv)
{
  entry = ee;
  LOG << "Standalone starting : " << argv[0] << std::endl;

  LOG << "CLAP Version : " << entry->clap_version.major << "." << entry->clap_version.major << "."
      << entry->clap_version.revision << std::endl;

  entry->init(argv[0]);

  auto fac = (const clap_plugin_factory *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  if (!fac)
  {
    LOG << "[ERROR] entry->get_factory return nullptr" << std::endl;
    return nullptr;
  }

  if (!standaloneHost)
  {
    standaloneHost = std::make_unique<StandaloneHost>();
  }

  if (clapId.empty())
  {
    LOG << "Loading CLAP by index " << clapIndex << std::endl;
    plugin = Clap::Plugin::createInstance(fac, clapIndex, standaloneHost.get());
  }
  else
  {
    LOG << "Loading CLAP by id '" << clapId << "'" << std::endl;
    plugin = Clap::Plugin::createInstance(fac, clapId, standaloneHost.get());
  }

  if (!plugin)
  {
    LOG << "[ERROR] Unable to create plugin" << std::endl;
    return nullptr;
  }

  standaloneHost->setPlugin(plugin);

  plugin->initialize();

  auto pt = getStandaloneSettingsPath();
  if (pt.has_value())
  {
    auto loadPath = *pt / plugin->_plugin->desc->id;

    try
    {
      LOG << "Trying to save default clap wrapper settings" << std::endl;
      standaloneHost->saveStandaloneAndPluginSettings(loadPath, "defaults.clapwrapper");
    }
    catch (const fs::filesystem_error &e)
    {
      // Oh well - whatcha gonna do?
    }

    try
    {
      if (fs::exists(loadPath / "settings.clapwrapper"))
      {
        LOG << "Trying to load from clap wrapper settings" << std::endl;
        standaloneHost->tryLoadStandaloneAndPluginSettings(loadPath, "settings.clapwrapper");
      }
    }
    catch (const fs::filesystem_error &e)
    {
      // Oh well - whatcha gonna do?
    }
  }

  return plugin;
}

void mainStartAudio()
{
  standaloneHost->startMIDIThread();
  standaloneHost->startAudioThread();
}

std::shared_ptr<Clap::Plugin> getMainPlugin()
{
  return plugin;
}

StandaloneHost *getStandaloneHost()
{
  if (!standaloneHost)
  {
    standaloneHost = std::make_unique<StandaloneHost>();
  }
  return standaloneHost.get();
}

int mainWait()
{
  while (standaloneHost->running)
  {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(1s);
  }
  return 0;
}

int mainFinish()
{
  LOG << "Shutting down" << std::endl;

  if (standaloneHost && plugin)
  {
    standaloneHost->stopAudioThread();
    standaloneHost->stopMIDIThread();

    auto pt = getStandaloneSettingsPath();
    if (pt.has_value())
    {
      auto savePath = *pt / plugin->_plugin->desc->id;
      LOG << "Saving settings to '" << savePath << "'" << std::endl;
      try
      {
        fs::create_directories(savePath);
        standaloneHost->saveStandaloneAndPluginSettings(savePath, "settings.clapwrapper");
      }
      catch (const fs::filesystem_error &e)
      {
        // Oh well - whatcha gonna do?
      }
    }
    else
    {
      LOG << "No Standalone Settings Path; not streaming" << std::endl;
    }

    plugin->deactivate();
  }
  plugin.reset();
  standaloneHost.reset();

  if (entry)
  {
    entry->deinit();
  }
  return 0;
}
}  // namespace freeaudio::clap_wrapper::standalone
