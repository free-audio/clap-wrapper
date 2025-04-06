#pragma once

#include "clap/private/macros.h"
#include <cstdint>


#ifdef __cplusplus
extern "C" {
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


#ifdef __cplusplus
}
#endif
