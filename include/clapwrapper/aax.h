#pragma once

#include "clap/private/macros.h"

/*
    Some information for the AAX factory/plugin structures can not
    be derived from the clap headers or structures. While in a pristine
    CLAP those information can be generated, you will need those informations
    fixed when maintaining compatibility with previously published
    versions.
*/

// CLAP_ABI was introduced in CLAP 1.1.2, for older versions we make it transparent
#ifndef CLAP_ABI
#define CLAP_ABI
#endif

// the factory extension
static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_AAX[] = "clap.plugin-factory-info-as-aax/0";

// the plugin extension
static const CLAP_CONSTEXPR char CLAP_PLUGIN_AS_AAX[] = "clap.plugin-info-as-aax/0";

typedef uint8_t array_of_16_bytes[16];

// clang-format off

// clang-format on

/*
  clap_plugin_as_aax

  all members are optional when set to nullptr
  if not provided, the wrapper code will use/generate appropriate values

  this struct is being returned by the plugin in clap_plugin_factory_as_aax::get_aax_info()
*/

typedef struct clap_plugin_info_as_aax
{
  const char* vendor;    // vendor
  const char* features;  // feature string for SubCategories
} clap_plugin_info_as_aax_t;

/*
  clap_plugin_factory_as_aax

  all members are optional and can be set to nullptr
  if not provided, the wrapper code will use/generate appropriate values

  retrieved when asking for factory CLAP_PLUGIN_FACTORY_INFO_AAX by clap_entry::get_factory()
*/

typedef struct clap_plugin_factory_as_aax
{
  const char* package_name;  // the package name, otherwise the first plugin name is being used
  uint32_t package_version;

  // retrieve additional information for the Steinberg::PClassInfo2 struct by pointer to clap_plugin_as_vst3
  // returns nullptr if no additional information is provided or can be a nullptr itself
  const clap_plugin_info_as_aax_t*(CLAP_ABI* get_vst3_info)(const clap_plugin_factory_as_aax* factory,
                                                            uint32_t index);

  bool(CLAP_ABI* can_apply_configuration)(const clap_plugin_t* plugin,
                                          const struct clap_audio_port_configuration_request* requests,
                                          uint32_t request_count);

} clap_plugin_factory_as_aax_t;

enum clap_supported_note_expressions_aax
{
  AS_AAX_NOTE_EXPRESSION_VOLUME = 1 << 0,
  AS_AAX_NOTE_EXPRESSION_PAN = 1 << 1,
  AS_AAX_NOTE_EXPRESSION_TUNING = 1 << 2,
  AS_AAX_NOTE_EXPRESSION_VIBRATO = 1 << 3,
  AS_AAX_NOTE_EXPRESSION_EXPRESSION = 1 << 4,
  AS_AAX_NOTE_EXPRESSION_BRIGHTNESS = 1 << 5,
  AS_AAX_NOTE_EXPRESSION_PRESSURE = 1 << 6,

  AS_AAX_NOTE_EXPRESSION_ALL = (1 << 7) - 1  // just the and of the above

};

/*
  retrieve additional information for the plugin itself, if note expressions are being supported and if there
  is a limit in MIDI channels (to reduce the offered controllers etc. in the AAX host)

  This extension is optionally returned by the plugin when asked for extension CLAP_PLUGIN_AS_AAX
*/
typedef struct clap_plugin_as_aax
{
  uint32_t(CLAP_ABI* getNumMIDIChannels)(const clap_plugin* plugin, uint32_t note_port);  // return 1-16
  uint32_t(CLAP_ABI* supportedNoteExpressions)(
      const clap_plugin* plugin);  // returns a bitmap of clap_supported_note_expressions
} clap_plugin_as_aax_t;
