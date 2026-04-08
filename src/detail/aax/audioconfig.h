#pragma once

#include <vector>
#include <string>

#include "clap_proxy.h"
#include "factory.h"

namespace CLAPAAX
{

typedef struct stemformat_combi
{
  std::string name;
  uint32_t format_in;
  uint32_t format_out;
  uint32_t plugin_id = 0;  // AAX_eProperty_PlugInID_Native override; 0 = auto-generate
} stemformat_combi_t;

typedef struct sAAXStemIndexToClapMap
{
  const char *identifier;
  uint32_t aaxStemformat;
  const uint8_t *clapmap;
  size_t mapsize;
} sAAXStemIndexToClapMap_t;

struct plugin_bus_info_t
{
  std::vector<stemformat_combi_t> stemformats;
  bool has_midi_in = false;
  bool has_midi_out = false;
  std::string midi_in_name;
  std::string midi_out_name;
};

plugin_bus_info_t getAvailableBusConfigs(Clap::Library *factory, uint32_t index);

}  // namespace CLAPAAX