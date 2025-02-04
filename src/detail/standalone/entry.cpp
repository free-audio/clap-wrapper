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
  LOGINFO("Standalone starting : {}", argv[0]);

  LOGINFO("CLAP Version : {}.{}.{}", entry->clap_version.major, entry->clap_version.major,
          entry->clap_version.revision);

  entry->init(argv[0]);

  auto fac = (const clap_plugin_factory *)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  if (!fac)
  {
    LOGINFO("[ERROR] entry->get_factory return nullptr");
    return nullptr;
  }

  if (!standaloneHost)
  {
    standaloneHost = std::make_unique<StandaloneHost>();
  }

  if (clapId.empty())
  {
    LOGINFO("Loading CLAP by index {}", clapIndex);
    plugin = Clap::Plugin::createInstance(fac, clapIndex, standaloneHost.get());
  }
  else
  {
    LOGINFO("Loading CLAP by id '{}'", clapId);
    plugin = Clap::Plugin::createInstance(fac, clapId, standaloneHost.get());
  }

  if (!plugin)
  {
    LOGINFO("[ERROR] Unable to create plugin");
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
      fs::create_directories(loadPath);
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
  LOGINFO("Shutting down");

  if (standaloneHost && plugin)
  {
    standaloneHost->stopAudioThread();
    standaloneHost->stopMIDIThread();

    auto pt = getStandaloneSettingsPath();
    if (pt.has_value())
    {
      auto savePath = *pt / plugin->_plugin->desc->id;
      LOGINFO("Saving settings to '{}'", savePath.u8string());
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
      LOGINFO("[WARNING] No Standalone Settings Path; not streaming");
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
