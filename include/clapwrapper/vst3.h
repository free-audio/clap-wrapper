#pragma once

#include "clap/private/macros.h"

/*
    some information for the VST3 factory/plugin structures can not 
    be derived from the clap headers. while in a pristine CLAP those
    information can be generated, you will need those informations
    fixed when maintaining compatibility with previously published
    versions.

*/

static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_VST3[] = "clap.plugin-factory-info-as-vst3.draft0";
static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_VST3[] = "clap.plugin-info-as-vst3.draft0";

typedef uint8_t array_of_16_bytes[16];

/*
  clap_plugin_as_vst3

  all members are optional and can be set to nullptr
  if not provided, the wrapper code will use/generate appropriate values
*/

typedef struct clap_plugin_as_vst3
{
  const char* vendor;                   // vendor
  const array_of_16_bytes* componentId; // compatibility GUID
  const char* features;                 // feature string for SubCategories
} clap_plugin_as_vst3_t;

/*
  clap_plugin_factory_as_vst3

  all members are optional and can be set to nullptr
  if not provided, the wrapper code will use/generate appropriate values
*/

typedef struct clap_plugin_factory_as_vst3
{
  // optional values for the Steinberg::PFactoryInfo structure
  const char* vendor;             // if not nullptr, the vendor string in the 
  const char* vendor_url;
  const char* email_contact;

  // retrieve additional information for the Steinberg::PClassInfo2 struct by pointer to clap_plugin_as_vst3
  // returns nullptr if no additional information is provided or can be a nullptr itself
  const clap_plugin_as_vst3_t* (*get_vst3_info)(const clap_plugin_factory_as_vst3* factory, uint32_t index);
} clap_plugin_factory_as_vst3_t;

