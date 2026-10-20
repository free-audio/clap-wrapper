#include <cstdio>
#include <cstring>
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

  // before the plugin exists, so its init() can already save the audio and midi setup
  std::string pluginId{clapId};
  if (pluginId.empty() && clapIndex < fac->get_plugin_count(fac))
  {
    auto desc = fac->get_plugin_descriptor(fac, clapIndex);
    if (desc && desc->id) pluginId = desc->id;
  }
  standaloneHost->resolveSettingsFolder(entry, fac, pluginId);
  // and the settings in it, so that what init() changes through the standalone features is
  // saved over the setup the user saved, not over an empty one
  standaloneHost->loadStandaloneSettings();

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

  if (standaloneHost->settingsFolder.has_value())
  {
    auto loadPath = *standaloneHost->settingsFolder;

    try
    {
      fs::create_directories(loadPath);
      standaloneHost->saveStandaloneAndPluginSettings(loadPath, "defaults.clapwrapper");
    }
    catch (const fs::filesystem_error &e)
    {
      // Oh well - whatcha gonna do?
    }

    // the last session's state, unless the plugin asked from init() for a fresh start
    try
    {
      if (standaloneHost->pluginStatePersistence == CLAP_WRAPPER_STANDALONE_STATE_PERSIST_SESSION &&
          fs::exists(loadPath / "settings.clapwrapper"))
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
  // audio first: startAudioThread() is what applies the saved settings, and the midi ports
  // opened before it were the defaults' - every input and no output - on every launch
  standaloneHost->startAudioThread();
  standaloneHost->startMIDIThread();
}

std::shared_ptr<Clap::Plugin> getMainPlugin()
{
  return plugin;
}

bool mainListAudioDevices(int argc, const char *const *argv)
{
  bool asked{false};
  for (int i = 1; i < argc && argv && argv[i]; ++i)
    asked = asked || !strcmp(argv[i], "--list-audio-devices");
  if (!asked) return false;

  auto join = [](const std::vector<std::string> &items)
  {
    std::string res;
    for (const auto &item : items) res += (res.empty() ? "" : ", ") + item;
    return res;
  };

  const auto apis{audioApis()};
  for (size_t a = 0; a < apis.size(); ++a)
  {
    printf("audio api %s (%s)%s\n", apis[a].id.c_str(), apis[a].name.c_str(),
           a == 0 ? ", the default" : "");

    auto backend{createAudioBackend(apis[a].id)};
    if (!backend) continue;

    const auto defaultIn{backend->defaultInputDevice()};
    const auto defaultOut{backend->defaultOutputDevice()};
    for (const auto &d : backend->devices())
    {
      std::vector<std::string> rates, flags, inNames, outNames;
      for (auto r : d.sampleRates) rates.push_back(std::to_string(r));
      for (uint32_t ch = 0; ch < d.inputChannels; ++ch) inNames.push_back(channelName(d, true, ch));
      for (uint32_t ch = 0; ch < d.outputChannels; ++ch) outNames.push_back(channelName(d, false, ch));
      if (d.id == defaultIn) flags.emplace_back("default input");
      if (d.id == defaultOut) flags.emplace_back("default output");
      if (d.isMicrophone) flags.emplace_back("microphone");
      if (d.isBluetooth) flags.emplace_back("bluetooth");
      if (d.isInterface) flags.emplace_back("interface");
      if (!d.sampleRateSettable) flags.emplace_back("rate not settable");

      printf("  \"%s\"\n", d.name.c_str());
      printf("    id: %s\n", d.id.c_str());
      printf("    legacy name: %s\n", d.legacyName.c_str());
      printf("    inputs: %u%s%s\n", d.inputChannels, d.inputChannels ? " - " : "",
             join(inNames).c_str());
      printf("    outputs: %u%s%s\n", d.outputChannels, d.outputChannels ? " - " : "",
             join(outNames).c_str());
      printf("    rates: %s (now %u)\n", join(rates).c_str(), d.currentSampleRate);
      printf("    buffer: %u to %u frames\n", d.minBufferFrames, d.maxBufferFrames);
      printf("    clock domain: %u\n", d.clockDomain);
      if (!flags.empty()) printf("    %s\n", join(flags).c_str());
    }
  }

  if (auto midi{createMidiBackend()})
  {
    printf("midi api %s\n", midi->apiId().c_str());
    for (bool input : {true, false})
    {
      for (const auto &port : input ? midi->inputs() : midi->outputs())
      {
        printf("  %s \"%s\"\n    id: %s\n    legacy name: %s\n", input ? "input" : "output",
               port.name.c_str(), port.id.c_str(), port.legacyName.c_str());
      }
    }
  }

  fflush(stdout);
  return true;
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

    if (standaloneHost->pluginStatePersistence != CLAP_WRAPPER_STANDALONE_STATE_PERSIST_SESSION)
    {
      LOGINFO("The plugin keeps no session; not streaming");
    }
    else if (standaloneHost->settingsFolder.has_value())
    {
      auto savePath = *standaloneHost->settingsFolder;
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

    standaloneHost->deactivatePlugin();
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
