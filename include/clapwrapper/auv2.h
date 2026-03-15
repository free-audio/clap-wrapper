#pragma once

#include "clap/private/macros.h"
#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

  static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_AUV2[] =
      "clap.plugin-factory-info-as-auv2.draft0";

  typedef struct clap_plugin_info_as_auv2
  {
    char au_type[5];  // the au_type. If empty (best choice) use the features[0] to aumu aufx aumi
    char au_subt[5];  // the subtype. If empty (worst choice) we try a bad 32 bit hash of the id
  } clap_plugin_info_as_auv2_t;

  typedef struct clap_plugin_factory_as_auv2
  {
    // optional values for the Steinberg::PFactoryInfo structure
    const char *manufacturer_code;  // your 'manu' field
    const char *manufacturer_name;  // your manufacturer display name

    // populate information about this particular auv2. If this method returns
    // false, the CLAP Plugin at the given index will not be exported into the
    // resulting AUv2
    bool(CLAP_ABI *get_auv2_info)(const clap_plugin_factory_as_auv2 *factory, uint32_t index,
                                  clap_plugin_info_as_auv2_t *info);
  } clap_plugin_factory_as_auv2_t;

  // Parameter order matters in auv2 critically still in logic and garage band, and if you add
  // parameters after a release, you need to order them (alas) even if the ids aren't changed.
  // clap_plugin_auv2_param_ordering extension allows you to provide an ordering for your params
  // by mapping the get_param_info index (0..num-params) to a different ordering.
  //
  // The result of this will be used such that `auv2_index = ordering[clap_index]`. So in a five
  // parameter case, if you want clap index 0 to appear at auv2 position 3, and clap index 3 to appear
  // at auv2 position 0, you would return `3 1 2 0 4`
  //
  // The default behavior absent this extension is to sort by auv2 parameter id, which is clap param id.
  // So if you use a strategy where you go from not adopting this to adopting this when you add params,
  // make sure your old version param subset retains the param id ordering.
  //
  // The clap wrapper will check if your ordering is complete and valid, and if not, generate errors
  // to stdout and, in a debug build, fail an assertion. Running in auval while developing this
  // method with a debug build enabled is helpful.
  //
  // A reasonable implementation if your parameters have a 'version' increasing parameter could be
  //
  // static bool CLAP_ABI auv2_get_param_order(const clap_plugin_t *plugin, size_t *order,
  //   size_t param_count) noexcept
  // {
  //   auto *self = static_cast<SixSinesClap *>(plugin->plugin_data);
  //   auto &params = self->engine->patch.params; // your internal model!
  //   if (param_count != params.size())
  //     return false;
  //
  //   std::iota(order, order + param_count, (size_t)0);
  //   std::sort(order, order + param_count, [&params](size_t a, size_t b) {
  //       bool aVer = params[a]->meta.version;
  //       bool bVer = params[b]->meta.version;
  //       if (aVer != bVer)
  //       {
  //           return aVer < bVer;
  //       }
  //       return params[a]->meta.id < params[b]->meta.id;
  //   });
  //   return true;
  // }
  static const CLAP_CONSTEXPR char CLAP_PLUGIN_AUV2_PARAM_ORDERING[] =
      "clap.plugin-auv2-param-ordering/0";

  typedef struct clap_plugin_auv2_param_ordering
  {
    // given an empty input array order of size param_count, populate it with the index ordering.
    // return true if successful. if successful, the order array will contain each index 0...param_count-1
    // once and only once and the param at auv2_index will be the clap param at ordering[clap_index].
    bool(CLAP_ABI *get_param_order)(const clap_plugin_t *plugin, size_t *order, size_t param_count);
  } clap_plugin_auv2_param_ordering_t;

#ifdef __cplusplus
}
#endif
