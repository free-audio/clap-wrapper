#include <memory>

#include "standalone_details.h"
#include "standalone_host.h"
#include "entry.h"

namespace Clap::Standalone
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

  standaloneHost = std::make_unique<StandaloneHost>();

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

  plugin->setSampleRate(48000);
  plugin->setBlockSizes(32, 1024);
  plugin->activate();

  plugin->start_processing();

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
}  // namespace Clap::Standalone