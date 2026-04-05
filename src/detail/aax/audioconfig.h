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
} stemformat_combi_t;

typedef struct sAAXStemIndexToClapMap
{
  const char *identifier;
  uint32_t aaxStemformat;
  const uint8_t *clapmap;
  size_t mapsize;
} sAAXStemIndexToClapMap_t;

std::vector<stemformat_combi_t> getAvailableBusConfigs(Clap::Library *factory, uint32_t index);

}  // namespace CLAPAAX