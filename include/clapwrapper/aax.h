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
static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_AAX[] = "clap.plugin-factory-info-as-aax/1";

// the plugin extension
static const CLAP_CONSTEXPR char CLAP_PLUGIN_AS_AAX[] = "clap.plugin-info-as-aax/1";

// clap_plugin_info_as_aax_t is being inquired for each plugin listed in the factory

typedef struct clap_plugin_aax_stem_config
{
  const char *name;
  uint32_t format_in;
  uint32_t format_out;
  uint32_t plugin_id;  // AAX_eProperty_PlugInID_Native override; 0 = auto-generate
} clap_plugin_aax_stem_config_t;

// this struct describes features for ONE plugin type.
// you can override the `uint32_t aax_features` by setting it to >0, otherwise the feature string will be parsed
// you can also set the config that will be reported via additional id
/*
  clap_plugin_as_aax

  this struct describes features for ONE plugin type.

  if not provided, the wrapper code will use/generate appropriate values.
  the function pointers are not optional.

  this struct is being returned by the plugin in clap_plugin_factory_as_aax::get_aax_info()

  the issue this shall solve is that AAX declares everything at factory time which is at
  plugin time in CLAP. the information is usually extracted at factory time by a miniclap host
  that instantiates each plugin in the clap factory once and reads out some information.

  to improve start up speed, a plugin writer can provide this information on factory time, too,
  by providing the information via the clap_plugin_factory_as_aax_t factory extension.

*/

typedef struct clap_plugin_info_as_aax
{
  uint32_t aax_features;  // maps directly the AAX_EPlugInCategory enum.

  uint32_t id_manufacturer;  // AAX_eProperty_ManufacturerID, should be registered with Avid
  uint32_t id_product;       // AAX_eProperty_ProductID,
  // leave them 0x00000000 when clap-wrapper shall generate this ID from the id strings automatically

  const char *midi_in_name;        // name of the MIDI IN, set to nullptr if no MIDI in
  const char *midi_out_name;       // name of the MIDI OUT, set to nullptr if no MIDI out
  uint32_t midi_in_channel_mask;   // channel mask for the MIDI IN
  uint32_t midi_out_channel_mask;  // channel mask for the MIDI OUT

  uint32_t(CLAP_ABI *get_num_stem_configs)();
  const clap_plugin_aax_stem_config_t *(CLAP_ABI *get_stem_config)(uint32_t index);

} clap_plugin_info_as_aax_t;

/*
  clap_plugin_factory_as_aax

  all members are optional and can be set to nullptr or 0
  if not provided, the wrapper code will use/generate appropriate values
  the function pointers are not optional.

  retrieved when asking for factory CLAP_PLUGIN_FACTORY_INFO_AAX by clap_entry::get_factory()
*/

typedef struct clap_plugin_factory_as_aax
{
  const char *package_name;          // the package name, otherwise the first plugin name is being used
  const char *package_manufacturer;  // the package vendor
  uint32_t package_version;          // the actual version

  // retrieve additional information for the AAX information like plugin/component ids for bus configs etc.
  // returns nullptr if no additional information is provided or can be a nullptr itself
  const clap_plugin_info_as_aax_t *(CLAP_ABI *get_aax_info)(const clap_plugin_factory_as_aax *factory,
                                                            uint32_t index);

  bool(CLAP_ABI *can_apply_configuration)(const clap_plugin_t *plugin,
                                          const struct clap_audio_port_configuration_request *requests,
                                          uint32_t request_count);

} clap_plugin_factory_as_aax_t;

#if 0
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
  uint32_t(CLAP_ABI *getNumMIDIChannels)(const clap_plugin *plugin, uint32_t note_port);  // return 1-16
  uint32_t(CLAP_ABI *supportedNoteExpressions)(
      const clap_plugin *plugin);  // returns a bitmap of clap_supported_note_expressions
} clap_plugin_as_aax_t;

#endif