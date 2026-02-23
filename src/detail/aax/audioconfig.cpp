#include "audioconfig.h"

// Describe
#include "AAX_IEffectDescriptor.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IPropertyMap.h"

#include "clap_proxy.h"
#include "factory.h"

std::vector<std::string> getAvailableBusConfigs(const char* pluginid)
{
  // the local microhost
  // why here? because we have factory and the clap-id
  static const clap_host_params_t micro_params = {
      [](const clap_host_t* host, clap_param_rescan_flags flags) -> void {},
      [](const clap_host_t* host, clap_id param_id, clap_param_clear_flags flags) -> void {},
      [](const clap_host_t* host) -> void {}};
  static const clap_host_audio_ports_t micro_audio_ports = {
      [](const clap_host_t* host, uint32_t flag) -> bool { return false; },
      [](const clap_host_t* host, uint32_t flags) -> void {}};
  clap_host_t microhost = {
      CLAP_VERSION,
      nullptr,
      "aax_scanner",
      "clap_wrapper",
      "",
      "1.0",
      [](const struct clap_host* host, const char* extension_id) -> const void*
      {
        if (extension_id == nullptr) return nullptr;
        os::log(extension_id);
        if (!strcmp(CLAP_EXT_PARAMS, extension_id)) return &micro_params;
        if (!strcmp(CLAP_EXT_AUDIO_PORTS, extension_id)) return &micro_audio_ports;
        return nullptr;
      },
      [](const struct clap_host* host) -> void {},  // request_restart
      [](const struct clap_host* host) -> void {},  // request_process
      [](const struct clap_host* host) -> void {},  // request_callback
  };

  // -------------------------------------------------------------------------

  /*
  * the audio configuration for each setting is encoded in a string.
  * this string will be prefixed to the plugin id, so the ClapAsAAX instance
  * can probably configure the busses before they start to render or the
  * instance is being activated.
  * 
  * The id stored in the AAX controller will be like this:
  * <busprefix>*|<actual-clapid>
  * 
  * <busprefix> format is <port-id>:<direction>:<type>:<channels>:<channel-map>
  * 
  * multiple busprefixes can be available
  * 
  */

  std::vector<std::string> result;

  auto factory = CLAPAAX::guarantee_clap();

  using configrequests_t = std::vector<clap_audio_port_configuration_request>;

  if (!factory->plugins.empty())
  {
    for (const auto i : factory->plugins)
    {
      std::vector<configrequests_t> configs;

      try
      {
        // create a temporary plugin instance ------------------
        auto* tmpplug =
            factory->_pluginFactory->create_plugin(factory->_pluginFactory, &microhost, i->id);
        try
        {
          tmpplug->init(tmpplug);

          //auto ext_aud =
          //    (clap_plugin_audio_ports*)(tmpplug->get_extension(tmpplug, CLAP_EXT_AUDIO_PORTS));
          //auto ext_cap =
          //  (clap_plugin_configurable_audio_ports_t*)(tmpplug->get_extension(tmpplug, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS));
          //auto ext_sur =
          //  (clap_plugin_surround_t*)(tmpplug->get_extension(tmpplug, CLAP_EXT_SURROUND));

          //if (!ext_sur) {
          //  // only standard audio bus
          // }
        }
        catch (...)
        {
          os::log("something got totally wrong");
        }
        tmpplug->destroy(tmpplug);
      }
      catch (std::exception& e)
      {
        os::log(e.what());
      }
    }
  }
  return result;
}