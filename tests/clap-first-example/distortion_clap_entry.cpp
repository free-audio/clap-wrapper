/*
 * distortion_clap_entry
 *
 * This uses extern versions of the factory methods from a static library to create
 * a working exported C linkage clap entry point
 */

#include <clap/clap.h>
#include <cstring>

extern uint32_t plugin_factory_get_plugin_count(const struct clap_plugin_factory *factory);
extern const clap_plugin_descriptor_t *plugin_factory_get_plugin_descriptor(
    const struct clap_plugin_factory *factory, uint32_t index);

extern const clap_plugin_t *plugin_factory_create_plugin(const struct clap_plugin_factory *factory,
                                                         const clap_host_t *host, const char *plugin_id);

static const clap_plugin_factory_t s_plugin_factory = {
    plugin_factory_get_plugin_count,
    plugin_factory_get_plugin_descriptor,
    plugin_factory_create_plugin,
};

////////////////
// clap_entry //
////////////////

static bool entry_init(const char *plugin_path)
{
  // called only once, and very first
  return true;
}

static void entry_deinit(void)
{
  // called before unloading the DSO
}

static const void *entry_get_factory(const char *factory_id)
{
  if (!strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &s_plugin_factory;
  return nullptr;
}

extern "C"
{
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"  // other peoples errors are outside my scope
#endif

  const CLAP_EXPORT struct clap_plugin_entry clap_entry = {CLAP_VERSION, entry_init, entry_deinit,
                                                           entry_get_factory};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}