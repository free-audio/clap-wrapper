#pragma once

#include "clap/private/macros.h"

/*
    Some information for the VST3 factory/plugin structures can not
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
static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_VST3[] = "clap.plugin-factory-info-as-vst3/0";

// the plugin extension
static const CLAP_CONSTEXPR char CLAP_PLUGIN_AS_VST3[] = "clap.plugin-info-as-vst3/0";

typedef uint8_t array_of_16_bytes[16];

// clang-format off
// VST3GUID allows you to provide the 4 uint32_t parts of the GUID and transforms them to the 16 byte array
#if WIN
#define VST3GUID(g1, g2, g3, g4) \
{                                \
(uint8_t)((g1 & 0x000000FF)      ),    \
(uint8_t)((g1 & 0x0000FF00) >>  8),    \
(uint8_t)((g1 & 0x00FF0000) >> 16),    \
(uint8_t)((g1 & 0xFF000000) >> 24),    \
(uint8_t)((g2 & 0x00FF0000) >> 16),    \
(uint8_t)((g2 & 0xFF000000) >> 24),    \
(uint8_t)((g2 & 0x000000FF)      ),    \
(uint8_t)((g2 & 0x0000FF00) >>  8),    \
(uint8_t)((g3 & 0xFF000000) >> 24),    \
(uint8_t)((g3 & 0x00FF0000) >> 16),    \
(uint8_t)((g3 & 0x0000FF00) >>  8),    \
(uint8_t)((g3 & 0x000000FF)      ),    \
(uint8_t)((g4 & 0xFF000000) >> 24),    \
(uint8_t)((g4 & 0x00FF0000) >> 16),    \
(uint8_t)((g4 & 0x0000FF00) >>  8),    \
(uint8_t)((g4 & 0x000000FF)      ),    \
}

#else
#define VST3GUID(g1, g2, g3, g4) \
{                                \
(uint8_t)((g1 & 0xFF000000) >> 24),    \
(uint8_t)((g1 & 0x00FF0000) >> 16),    \
(uint8_t)((g1 & 0x0000FF00) >>  8),    \
(uint8_t)((g1 & 0x000000FF)      ),    \
(uint8_t)((g2 & 0xFF000000) >> 24),    \
(uint8_t)((g2 & 0x00FF0000) >> 16),    \
(uint8_t)((g2 & 0x0000FF00) >>  8),    \
(uint8_t)((g2 & 0x000000FF)      ),    \
(uint8_t)((g3 & 0xFF000000) >> 24),    \
(uint8_t)((g3 & 0x00FF0000) >> 16),    \
(uint8_t)((g3 & 0x0000FF00) >>  8),    \
(uint8_t)((g3 & 0x000000FF)      ),    \
(uint8_t)((g4 & 0xFF000000) >> 24),    \
(uint8_t)((g4 & 0x00FF0000) >> 16),    \
(uint8_t)((g4 & 0x0000FF00) >>  8),    \
(uint8_t)((g4 & 0x000000FF)      ),    \
}

#endif

// clang-format on

/*
  clap_plugin_as_vst3

  all members are optional when set to nullptr
  if not provided, the wrapper code will use/generate appropriate values

  this struct is being returned by the plugin in clap_plugin_factory_as_vst3_t::get_vst3_info()
*/

typedef struct clap_plugin_info_as_vst3
{
  const char* vendor;                    // vendor
  const array_of_16_bytes* componentId;  // compatibility GUID
  const char* features;                  // feature string for SubCategories
} clap_plugin_info_as_vst3_t;

/*
  clap_plugin_factory_as_vst3

  all members are optional and can be set to nullptr
  if not provided, the wrapper code will use/generate appropriate values

  retrieved when asking for factory CLAP_PLUGIN_FACTORY_INFO_VST3 by clap_entry::get_factory()
*/

typedef struct clap_plugin_factory_as_vst3
{
  // optional values for the Steinberg::PFactoryInfo structure
  const char* vendor;  // if not nullptr, the vendor string in the
  const char* vendor_url;
  const char* email_contact;

  // retrieve additional information for the Steinberg::PClassInfo2 struct by pointer to clap_plugin_as_vst3
  // returns nullptr if no additional information is provided or can be a nullptr itself
  const clap_plugin_info_as_vst3_t*(CLAP_ABI* get_vst3_info)(const clap_plugin_factory_as_vst3* factory,
                                                             uint32_t index);
} clap_plugin_factory_as_vst3_t;

enum clap_supported_note_expressions
{
  AS_VST3_NOTE_EXPRESSION_VOLUME = 1 << 0,
  AS_VST3_NOTE_EXPRESSION_PAN = 1 << 1,
  AS_VST3_NOTE_EXPRESSION_TUNING = 1 << 2,
  AS_VST3_NOTE_EXPRESSION_VIBRATO = 1 << 3,
  AS_VST3_NOTE_EXPRESSION_EXPRESSION = 1 << 4,
  AS_VST3_NOTE_EXPRESSION_BRIGHTNESS = 1 << 5,
  AS_VST3_NOTE_EXPRESSION_PRESSURE = 1 << 6,

  AS_VST3_NOTE_EXPRESSION_ALL = (1 << 7) - 1  // just the and of the above

};

/*
  retrieve additional information for the plugin itself, if note expressions are being supported and if there
  is a limit in MIDI channels (to reduce the offered controllers etc. in the VST3 host)

  This extension is optionally returned by the plugin when asked for extension CLAP_PLUGIN_AS_VST3
*/
typedef struct clap_plugin_as_vst3
{
  uint32_t(CLAP_ABI* getNumMIDIChannels)(const clap_plugin* plugin, uint32_t note_port);  // return 1-16
  uint32_t(CLAP_ABI* supportedNoteExpressions)(
      const clap_plugin* plugin);  // returns a bitmap of clap_supported_note_expressions
} clap_plugin_as_vst3_t;
