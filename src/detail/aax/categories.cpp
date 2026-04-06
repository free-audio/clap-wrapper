// AAX
// converts CLAP plugin categories to AAX category bitfield

#include <stdint.h>
#include <AAX_Enums.h>
#include <clap/clap.h>
#include <vector>
#include <algorithm>
#include "../os/osutil.h"

// clang-format off

static const struct _translation
{
  const char* clapattribute;
  uint32_t aaxattribute;
} translationTable[] = {
    {CLAP_PLUGIN_FEATURE_INSTRUMENT       , AAX_ePlugInCategory_SWGenerators },
    {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT     , AAX_EPlugInCategory_Effect},
    {CLAP_PLUGIN_FEATURE_NOTE_EFFECT      , AAX_EPlugInCategory_MIDIEffect},
    {CLAP_PLUGIN_FEATURE_DRUM             , AAX_EPlugInCategory_MIDIEffect},
    {CLAP_PLUGIN_FEATURE_ANALYZER         , AAX_ePlugInCategory_NoiseReduction},

    // CLAP sub categories
    {CLAP_PLUGIN_FEATURE_SYNTHESIZER      , AAX_ePlugInCategory_SWGenerators},
    {CLAP_PLUGIN_FEATURE_SAMPLER          , AAX_ePlugInCategory_SWGenerators},
    {CLAP_PLUGIN_FEATURE_DRUM             , AAX_ePlugInCategory_SWGenerators},
    {CLAP_PLUGIN_FEATURE_DRUM_MACHINE     , AAX_ePlugInCategory_SWGenerators},

    {CLAP_PLUGIN_FEATURE_FILTER           , AAX_ePlugInCategory_Modulation},
    {CLAP_PLUGIN_FEATURE_PHASER           , AAX_ePlugInCategory_Modulation},
    {CLAP_PLUGIN_FEATURE_EQUALIZER        , AAX_ePlugInCategory_EQ},
    {CLAP_PLUGIN_FEATURE_DEESSER          , AAX_ePlugInCategory_NoiseReduction},
    {CLAP_PLUGIN_FEATURE_PHASE_VOCODER    , AAX_ePlugInCategory_Harmonic},
    {CLAP_PLUGIN_FEATURE_GRANULAR         , AAX_ePlugInCategory_SWGenerators},
    {CLAP_PLUGIN_FEATURE_FREQUENCY_SHIFTER, AAX_ePlugInCategory_Dynamics},
    {CLAP_PLUGIN_FEATURE_PITCH_SHIFTER    , AAX_ePlugInCategory_PitchShift},

    {CLAP_PLUGIN_FEATURE_DISTORTION       , AAX_ePlugInCategory_Harmonic},
    {CLAP_PLUGIN_FEATURE_TRANSIENT_SHAPER , AAX_ePlugInCategory_Harmonic},
    {CLAP_PLUGIN_FEATURE_COMPRESSOR       , AAX_ePlugInCategory_Dynamics},
    {CLAP_PLUGIN_FEATURE_LIMITER          , AAX_ePlugInCategory_Dynamics},

    {CLAP_PLUGIN_FEATURE_FLANGER          , AAX_ePlugInCategory_Modulation},
    {CLAP_PLUGIN_FEATURE_CHORUS           , AAX_ePlugInCategory_Modulation},

    {CLAP_PLUGIN_FEATURE_DELAY            , AAX_ePlugInCategory_Delay},
    {CLAP_PLUGIN_FEATURE_REVERB           , AAX_ePlugInCategory_Reverb},

    {CLAP_PLUGIN_FEATURE_TREMOLO          , AAX_ePlugInCategory_Modulation},
    {CLAP_PLUGIN_FEATURE_GLITCH           , AAX_ePlugInCategory_Modulation},

    {CLAP_PLUGIN_FEATURE_UTILITY          , AAX_EPlugInCategory_Effect},
    {CLAP_PLUGIN_FEATURE_PITCH_CORRECTION , AAX_ePlugInCategory_PitchShift},
    {CLAP_PLUGIN_FEATURE_RESTORATION      , AAX_ePlugInCategory_NoiseReduction},

    {CLAP_PLUGIN_FEATURE_MULTI_EFFECTS    , AAX_EPlugInCategory_Effect},

    {CLAP_PLUGIN_FEATURE_MIXING           , AAX_ePlugInCategory_SoundField},
    {CLAP_PLUGIN_FEATURE_MASTERING        , AAX_ePlugInCategory_SoundField},
    {"external"                           , AAX_ePlugInCategory_HWGenerators},

};

    /*{   CLAP_PLUGIN_FEATURE_ARA_SUPPORTED         , "OnlyARA" }, not defined for AAX yet */
    /*{CLAP_PLUGIN_FEATURE_ARA_REQUIRED          , "OnlyARA"}, */

// clang-format on

/*
*     clapCategoriesToAAX converts the strings from the CLAP attributes to the
*     AAX bitfield values AAX_ePluginCategory_*
* 
*     All attributes get applied to an OR operation, AAX has only a 32bit field
* 
*/
uint32_t clapCategoriesToAAX(const char *const *clap_categories)
{
  // AAX_ePlugInCategory_WrappedPlugin = 0x00001000,	///<  All plug-ins wrapped by a thrid party wrapper (i.e. VST to RTAS wrapper), except for virtual instrument plug-ins which should be mapped to AAX_PlugInCategory_SWGenerators
  uint32_t result = 0;  // we don't use AAX_ePlugInCategory_WrappedPlugin;
  LOGDETAIL("creating categories:");
  for (auto f = clap_categories; f && *f; ++f)
  {
    auto it =
        std::find_if(std::begin(translationTable), std::end(translationTable), [&](const auto &entry)
                     { return entry.clapattribute && !strcmp(entry.clapattribute, *f); });

    if (it != std::end(translationTable))
    {
      LOGDETAIL(fmt::format("  {}", it->clapattribute));
      result |= it->aaxattribute;
    }
  }
  return result;
}
