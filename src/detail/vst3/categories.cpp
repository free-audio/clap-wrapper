/*
    converting CLAP categories to VST3 categories
    
    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.


    both, CLAP and VST3, use strings to describe the plugin type.
    CLAP uses 4 main categories and a bunch of sub categories, where there is no limit of that list.

    VST3 uses a string where the categories are separated by '|'. The translation table below
    allows to match CLAP categories to VST3 categories. It is intended that a CLAP feature string
    could appear twice in the list with a different, additional VST3 string, so a Flanger would provide
    this list:

      audio-effect
      flanger

    which could result in VST3 as

      Fx|Modulation|Flanger

    Also the "Fx" could be added multiple times, duplicates will be automatically removed before creating
    the VST3 category string without changing the order.

    Note that most VST3 hosts will use the first token (like `Fx` or `Instrument`) to locate the plugin
    in a specific location and the second token (like `Synth` as a sub menu category in selection menus).

    Additionally, the Steinberg::PClassInfo struct reserves 128 bytes for the subcategory string, so any
    category string that exceeds this limit won't be added anymore and no further strings will be added.
    Make sure that the two most important categories are always at the beginning of your CLAP descriptor.

    Note: If you as a plugin developer want to set the VST3 categories explicitely, you can use the
    CLAP_PLUGIN_AS_VST3 extension (see clap-wrapper/include/clapwrapper/vst3.h) to explicitely set
    the category string.

*/

#include "categories.h"
#include <vector>
#include <algorithm>
#include <clap/plugin-features.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

static const struct _translate
{
  const char* clapattribute;
  const char* vst3attribute;
} translationTable[] =
{
  // CLAP main categories
  {   CLAP_PLUGIN_FEATURE_INSTRUMENT            , PlugType::kInstrument },
  {   CLAP_PLUGIN_FEATURE_AUDIO_EFFECT          , PlugType::kFx},
  {   CLAP_PLUGIN_FEATURE_NOTE_EFFECT           , PlugType::kInstrumentSynth}, // it seems there is no type for a sequencer etc
  {   CLAP_PLUGIN_FEATURE_DRUM                  , PlugType::kInstrumentDrum},
  {   CLAP_PLUGIN_FEATURE_ANALYZER              , PlugType::kAnalyzer},

  // CLAP sub categories
  {   CLAP_PLUGIN_FEATURE_SYNTHESIZER           , "Synth"},
  {   CLAP_PLUGIN_FEATURE_SAMPLER               , "Sampler"},
  {   CLAP_PLUGIN_FEATURE_DRUM                  , "Drum"},
  {   CLAP_PLUGIN_FEATURE_DRUM_MACHINE          , "Drum"},

  {   CLAP_PLUGIN_FEATURE_FILTER                , "Filter"},
  {   CLAP_PLUGIN_FEATURE_PHASER                , "Modulation"          },
  {   CLAP_PLUGIN_FEATURE_EQUALIZER             , "EQ"},
  {   CLAP_PLUGIN_FEATURE_DEESSER               , "Restoration"},
  {   CLAP_PLUGIN_FEATURE_PHASE_VOCODER         , "Modulation"},
  {   CLAP_PLUGIN_FEATURE_GRANULAR              , "Synth"},
  {   CLAP_PLUGIN_FEATURE_FREQUENCY_SHIFTER     , "Modulator"},
  {   CLAP_PLUGIN_FEATURE_PITCH_SHIFTER         , "Pitch Shifter"},

  {   CLAP_PLUGIN_FEATURE_DISTORTION            , "Distortion"},
  {   CLAP_PLUGIN_FEATURE_TRANSIENT_SHAPER      , "Distortion"},
  {   CLAP_PLUGIN_FEATURE_COMPRESSOR            , "Dynamics"},
  {   CLAP_PLUGIN_FEATURE_LIMITER               , "Dynamics"},

  {   CLAP_PLUGIN_FEATURE_FLANGER               , "Modulation"},
  // {   CLAP_PLUGIN_FEATURE_FLANGER               , "Flanger"},
  {   CLAP_PLUGIN_FEATURE_CHORUS                , "Modulation"},
  // {   CLAP_PLUGIN_FEATURE_CHORUS                , "Chorus"},
  {   CLAP_PLUGIN_FEATURE_DELAY                 , "Delay"},
  {   CLAP_PLUGIN_FEATURE_REVERB                , "Reverb"},

  {   CLAP_PLUGIN_FEATURE_TREMOLO               , "Modulation"},
  {   CLAP_PLUGIN_FEATURE_GLITCH                , "Modulation"},

  {   CLAP_PLUGIN_FEATURE_UTILITY               , "Tools"},
  {   CLAP_PLUGIN_FEATURE_PITCH_CORRECTION      , "Pitch Shift"},
  {   CLAP_PLUGIN_FEATURE_RESTORATION           , "Restoration"},

  {   CLAP_PLUGIN_FEATURE_MULTI_EFFECTS         , "Tools"},

  {   CLAP_PLUGIN_FEATURE_MIXING                , "Mixing"},
  {   CLAP_PLUGIN_FEATURE_MASTERING             , "Mastering"},

  {   "external"                                , "External"},

  {nullptr, nullptr}
};

std::string clapCategoriesToVST3(const char* const* clap_categories)
{
  std::vector<std::string> r;
  auto f = clap_categories;
  while (f && *f)
  {
    int i = 0;
    while (translationTable[i].clapattribute)
    {
      if (!strcmp(translationTable[i].clapattribute, *f))
      {
        r.push_back(translationTable[i].vst3attribute);
      }
      ++i;
    }
    ++f;
  }
  std::vector<std::string> r2;
  for (auto& i : r)
  {
    if (std::find(r2.begin(), r2.end(), i) == r2.end())
    {
      r2.push_back(i);
    }
  }  
  
  std::string result;
  for (auto& i : r2)
  {
    if (result.size() + i.size() <= Steinberg::PClassInfo2::kSubCategoriesSize)
    {
      result.append(i);
      result.append("|");
    }
    else
    {
      result.append("*");
      break;
    }
  }
  result.pop_back();
  return result;

}