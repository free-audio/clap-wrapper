/*
    VST3 speaker arrangements <-> CLAP channel maps

    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    see channelmap.h for details
*/

#include "channelmap.h"

namespace Clap::Vst3Channels
{
using namespace Steinberg;

// clang-format off

  // VST3 speaker <-> CLAP surround position.
  //
  // The table is a bijection, which is what makes the round trip stable: an arrangement
  // sent to the plugin through apply_configuration() comes back unchanged from
  // get_channel_map().
  //
  // Two of the VST3 names need a word:
  //
  // ** Ls/Rs ("Left/Right Surround") -> BL/BR, Sl/Sr ("Side Left/Right") -> SL/SR
  // VST3's 7.1 Music uses both pairs at once (Ls Rs Sl Sr), so Ls/Rs has to be the back
  // pair. That is also how WAVE and ITU read the surround pair of a 5.1 layout. Note that
  // the AAX wrapper made the other choice for 5.1 (see detail/aax/audioconfig.cpp, where
  // Ls/Rs go to SL/SR); it has no 7.1 Music to keep consistent with.
  //
  // ** Cs ("Center of Surround") is the same bit as S ("Surround") -> BC
  //
  // The VST3 speakers CLAP has no position for are left out on purpose: Lfe2, the back
  // center pair Lcs/Rcs, the whole bottom layer (Bfl .. Brr), the proximity pair Pl/Pr and
  // the ambisonic components. An arrangement using one of them cannot be described to the
  // plugin, and the caller falls back to the channel count alone.

  static const struct
  {
    Vst::Speaker vst3;
    uint8_t clap;
  } kSpeakers[] = {
      {Vst::kSpeakerL,   CLAP_SURROUND_FL},
      {Vst::kSpeakerR,   CLAP_SURROUND_FR},
      {Vst::kSpeakerC,   CLAP_SURROUND_FC},
      {Vst::kSpeakerLfe, CLAP_SURROUND_LFE},
      {Vst::kSpeakerLs,  CLAP_SURROUND_BL},
      {Vst::kSpeakerRs,  CLAP_SURROUND_BR},
      {Vst::kSpeakerLc,  CLAP_SURROUND_FLC},
      {Vst::kSpeakerRc,  CLAP_SURROUND_FRC},
      {Vst::kSpeakerCs,  CLAP_SURROUND_BC},
      {Vst::kSpeakerSl,  CLAP_SURROUND_SL},
      {Vst::kSpeakerSr,  CLAP_SURROUND_SR},
      {Vst::kSpeakerTc,  CLAP_SURROUND_TC},
      {Vst::kSpeakerTfl, CLAP_SURROUND_TFL},
      {Vst::kSpeakerTfc, CLAP_SURROUND_TFC},
      {Vst::kSpeakerTfr, CLAP_SURROUND_TFR},
      {Vst::kSpeakerTrl, CLAP_SURROUND_TBL},
      {Vst::kSpeakerTrc, CLAP_SURROUND_TBC},
      {Vst::kSpeakerTrr, CLAP_SURROUND_TBR},
      {Vst::kSpeakerTsl, CLAP_SURROUND_TSL},
      {Vst::kSpeakerTsr, CLAP_SURROUND_TSR},
  };

// clang-format on

static bool clapPositionOf(Vst::Speaker speaker, uint8_t &position)
{
  for (const auto &entry : kSpeakers)
  {
    if (entry.vst3 == speaker)
    {
      position = entry.clap;
      return true;
    }
  }
  return false;
}

static Vst::Speaker speakerOf(uint8_t position)
{
  for (const auto &entry : kSpeakers)
  {
    if (entry.clap == position)
    {
      return entry.vst3;
    }
  }
  return 0;
}

bool channelMapFromArrangement(Vst::SpeakerArrangement arr, std::vector<uint8_t> &map)
{
  auto channelCount = Vst::SpeakerArr::getChannelCount(arr);
  if (channelCount <= 0)
  {
    return false;
  }

  std::vector<uint8_t> result;
  result.reserve(static_cast<size_t>(channelCount));
  for (int32 channel = 0; channel < channelCount; ++channel)
  {
    uint8_t position;
    if (!clapPositionOf(Vst::SpeakerArr::getSpeaker(arr, channel), position))
    {
      return false;
    }
    result.push_back(position);
  }

  map = std::move(result);
  return true;
}

Vst::SpeakerArrangement arrangementFromChannelMap(const uint8_t *map, uint32_t count)
{
  if (!map || count == 0)
  {
    return Vst::SpeakerArr::kEmpty;
  }

  Vst::SpeakerArrangement arr = Vst::SpeakerArr::kEmpty;
  Vst::Speaker previous = 0;
  for (uint32_t channel = 0; channel < count; ++channel)
  {
    auto speaker = speakerOf(map[channel]);

    // a position VST3 does not have, the same position twice, or a channel order that no
    // VST3 bus can express - its channels always run in ascending speaker order, and the
    // wrapper hands the host's channel pointers to the plugin in that order
    if (speaker == 0 || speaker <= previous)
    {
      return Vst::SpeakerArr::kEmpty;
    }

    arr |= speaker;
    previous = speaker;
  }

  return arr;
}

bool ambisonicConfigFromArrangement(Vst::SpeakerArrangement arr, clap_ambisonic_config_t &config)
{
  if (!Vst::SpeakerArr::isAmbisonics(arr))
  {
    return false;
  }

  // VST3 names the ordering in the arrangement itself (kAmbi*OrderACN) but says nothing
  // about the normalization, so SN3D - the AmbiX convention - is what is passed on.
  config.ordering = CLAP_AMBISONIC_ORDERING_ACN;
  config.normalization = CLAP_AMBISONIC_NORMALIZATION_SN3D;
  return true;
}

Vst::SpeakerArrangement arrangementFromAmbisonicConfig(const clap_ambisonic_config_t &config,
                                                       uint32_t channel_count)
{
  // FuMa has no VST3 arrangement, and neither has anything above the 3rd order. The
  // normalization cannot be carried over either, so a plugin that does not use SN3D is
  // described as if it did - VST3 has no way to say otherwise.
  if (config.ordering != CLAP_AMBISONIC_ORDERING_ACN)
  {
    return Vst::SpeakerArr::kEmpty;
  }

  switch (channel_count)
  {
    case 4:
      return Vst::SpeakerArr::kAmbi1stOrderACN;
    case 9:
      return Vst::SpeakerArr::kAmbi2cdOrderACN;
    case 16:
      return Vst::SpeakerArr::kAmbi3rdOrderACN;
    default:
      return Vst::SpeakerArr::kEmpty;
  }
}
}  // namespace Clap::Vst3Channels
