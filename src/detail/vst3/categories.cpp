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

    A VST3 attribute in the table can be a combination itself (like `Fx|Analyzer`), those are split
    up into their tokens, so the "Fx" can be added multiple times - duplicates are removed before
    creating the VST3 category string.

    Note that most VST3 hosts will use the first token (like `Fx` or `Instrument`) to locate the plugin
    in a specific location and the second token (like `Synth` as a sub menu category in selection menus).
    The tokens therefore keep the order of your CLAP features, with two exceptions: a main category
    (`Fx` or `Instrument`) is always moved to the front and tokens which describe a trait rather than
    a category (`OnlyARA`, `External`, ...) are always moved to the end, because neither of them may
    take the sub menu slot. Everything in between is up to you - make sure that the most important
    sub category comes first in your CLAP descriptor.

    Additionally, the Steinberg::PClassInfo struct reserves 128 bytes for the subcategory string
    (including its terminating zero), so any category that does not fit in anymore is dropped and no
    further categories will be added - which drops the least important ones for the same reason.

    Note: If you as a plugin developer want to set the VST3 categories explicitely, you can use the
    CLAP_PLUGIN_AS_VST3 extension (see clap-wrapper/include/clapwrapper/vst3.h) to explicitely set
    the category string.

*/

#include "categories.h"
#include <vector>
#include <algorithm>
#include <string>
#include <cstring>
#include <clap/plugin-features.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include "../ara/ara.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

// clang-format off
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
  // kAnalyzer alone ("Analyzer") is documented as "not selectable as insert plug-in", so a CLAP
  // analyzer would not be loadable in some hosts at all - kFxAnalyzer ("Fx|Analyzer") is the
  // insertable variant and still carries the analyzer sub category
  {   CLAP_PLUGIN_FEATURE_ANALYZER              , PlugType::kFxAnalyzer},

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

/*{   CLAP_PLUGIN_FEATURE_ARA_SUPPORTED         , PlugType::kOnlyARA }, this is indicated by a missing factory in VST3 */
  {   CLAP_PLUGIN_FEATURE_ARA_REQUIRED          , PlugType::kOnlyARA },

  {   "external"                                , "External"},

  {nullptr, nullptr}
};
// clang-format on

// the main categories - a VST3 has to start with one of these to be filed correctly by a host
static const char *const leadingCategories[] = {PlugType::kFx, PlugType::kInstrument};

// these describe a property of the plugin instead of a category, so they must never end up in
// the sub menu slot right behind the main category
static const char *const trailingCategories[] = {PlugType::kOnlyARA,      PlugType::kOnlyOfflineProcess,
                                                 PlugType::kOnlyRealTime, PlugType::kMono,
                                                 PlugType::kStereo,       "External"};

static int categoryRank(const std::string &category)
{
  for (const auto *c : leadingCategories)
  {
    if (category == c) return 0;
  }
  for (const auto *c : trailingCategories)
  {
    if (category == c) return 2;
  }
  return 1;
}

std::string clapCategoriesToVST3(const char *const *clap_categories)
{
  std::vector<std::string> tokens;

  // a VST3 attribute can be a combination itself (like `Fx|Analyzer`), so it is split up into its
  // tokens here - otherwise they could neither be ordered nor deduplicated. A token is added in
  // the order it was encountered and only once, so the first CLAP feature asking for it decides
  // its position.
  auto addTokens = [&tokens](const char *vst3attribute)
  {
    std::string attribute(vst3attribute);
    for (std::string::size_type pos = 0; pos < attribute.size();)
    {
      auto end = attribute.find('|', pos);
      if (end == std::string::npos) end = attribute.size();
      if (end > pos)
      {
        std::string token(attribute, pos, end - pos);
        if (std::find(tokens.begin(), tokens.end(), token) == tokens.end())
        {
          tokens.push_back(std::move(token));
        }
      }
      pos = end + 1;
    }
  };

  for (auto f = clap_categories; f && *f; ++f)
  {
    // a CLAP feature can appear more than once in the table to contribute additional VST3 tokens
    for (const auto &entry : translationTable)
    {
      if (entry.clapattribute && !strcmp(entry.clapattribute, *f))
      {
        addTokens(entry.vst3attribute);
      }
    }
  }

  // Move the main categories to the front and the traits to the back. The sort is stable, so
  // everything in between stays in the order the CLAP descriptor listed its features in.
  std::stable_sort(tokens.begin(), tokens.end(), [](const std::string &a, const std::string &b)
                   { return categoryRank(a) < categoryRank(b); });

  // the subcategory field is a fixed size buffer which has to hold the terminating zero as well
  constexpr std::string::size_type maxLength = Steinberg::PClassInfo2::kSubCategoriesSize - 1;

  std::string result;
  for (const auto &token : tokens)
  {
    std::string::size_type separator = result.empty() ? 0 : 1;
    if (result.size() + separator + token.size() > maxLength)
    {
      break;
    }
    if (separator != 0)
    {
      result.append("|");
    }
    result.append(token);
  }
  return result;
}
