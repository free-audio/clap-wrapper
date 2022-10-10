#pragma once

#include "clap/private/macros.h"

/*
    Some information for the VST3 factory/plugin structures can not 
    be derived from the clap headers or structures. While in a pristine
    CLAP those information can be generated, you will need those informations
    fixed when maintaining compatibility with previously published
    versions.
*/

// the factory extension
static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_VST3[] = "clap.plugin-factory-info-as-vst3.draft0";

// the plugin extension
static const CLAP_CONSTEXPR char CLAP_PLUGIN_AS_VST3[] = "clap.plugin-info-as-vst3.draft0";

typedef uint8_t array_of_16_bytes[16];

/*
  clap_plugin_as_vst3

  all members are optional when set to nullptr
  if not provided, the wrapper code will use/generate appropriate values
*/

typedef struct clap_plugin_info_as_vst3
{
  const char* vendor;                   // vendor
  const array_of_16_bytes* componentId; // compatibility GUID
  const char* features;                 // feature string for SubCategories
} clap_plugin_info_as_vst3_t;

/*
  clap_plugin_factory_as_vst3

  all members are optional and can be set to nullptr
  if not provided, the wrapper code will use/generate appropriate values

  retrieved when asking for factory CLAP_PLUGIN_FACTORY_INFO_VST3
*/

typedef struct clap_plugin_factory_as_vst3
{
  // optional values for the Steinberg::PFactoryInfo structure
  const char* vendor;             // if not nullptr, the vendor string in the 
  const char* vendor_url;
  const char* email_contact;

  // retrieve additional information for the Steinberg::PClassInfo2 struct by pointer to clap_plugin_as_vst3
  // returns nullptr if no additional information is provided or can be a nullptr itself
  const clap_plugin_info_as_vst3_t* (*get_vst3_info)(const clap_plugin_factory_as_vst3* factory, uint32_t index);
} clap_plugin_factory_as_vst3_t;

enum clap_supported_note_expressions
{
  AS_VST3_NOTE_EXPRESSION_VOLUME       = 1 << 0,
  AS_VST3_NOTE_EXPRESSION_PAN          = 1 << 1,
  AS_VST3_NOTE_EXPRESSION_TUNING       = 1 << 2,
  AS_VST3_NOTE_EXPRESSION_VIBRATO      = 1 << 3,
  AS_VST3_NOTE_EXPRESSION_EXPRESSION   = 1 << 4,
  AS_VST3_NOTE_EXPRESSION_BRIGHTNESS   = 1 << 5,
  AS_VST3_NOTE_EXPRESSION_PRESSURE     = 1 << 6
};

/*
  retrieve additional information for the plugin itself, if note expressions are being supported and if there
  is a limit in MIDI channels (to reduce the offered controllers etc. in the VST3 host)
*/
typedef struct clap_plugin_as_vst3
{
  uint32_t (*getNumMIDIChannels) (const clap_plugin* plugin, uint32_t note_port); // return 1-16
  uint32_t (*supportedNoteExpressions) (const clap_plugin* plugin); // returns a bitmap of clap_supported_note_expressions
} clap_plugin_as_vst3_t;

