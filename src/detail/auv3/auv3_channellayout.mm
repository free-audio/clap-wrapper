/*
    AUv3 channel layouts <-> CLAP channel maps

    Copyright (c) 2026 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    see auv3_channellayout.h for details
*/

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
// the HOA and Atmos tags and labels are plain constants: using them needs no OS check
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

#include "auv3_channellayout.h"

#include <cstddef>

namespace Clap::AUv3Channels
{

// clang-format off

  // CoreAudio channel label <-> CLAP surround position.
  //
  // The surround pairs need a word, because CoreAudio names them by context:
  //
  // ** Ls/Rs ("Left/Right Surround") is the surround pair of a 5.1 layout and the side
  // pair of a 7.1 layout, where Rls/Rrs ("Rear Surround") are the back pair - see
  // kAudioChannelLayoutTag_MPEG_7_1_C or the Atmos tags. So Ls/Rs -> BL/BR, the way the
  // VST3 wrapper reads the 5.1 pair, unless the layout also has the rear pair, then
  // Ls/Rs -> SL/SR and Rls/Rrs -> BL/BR. Lsd/Rsd ("Surround Direct") are side channels
  // wherever they appear.
  //
  // ** The other way round, SL/SR always become Ls/Rs, and BL/BR become Rls/Rrs when there
  // are side channels as well and Ls/Rs when there are not - which is what lets the tag
  // lookup find the usual layouts. A 5.1 with side channels therefore comes back as
  // Ls/Rs, and is sent to the plugin as BL/BR: a plugin that insists on SL/SR there
  // refuses the described request and gets the channel count, as before.
  //
  // ** Top channels: CoreAudio has two labels for each of the top back channels, the old
  // TopBack* and the *TopRear of the Atmos layouts. Both are read; the Atmos label is the
  // one written, so that the Atmos tags are found.
  //
  // The labels CLAP has no position for are left out on purpose: LFE2, the wide pair, the
  // rear center pair, the bottom layer, the headphone and matrix labels and the discrete
  // channels. A layout using one of them cannot be described to the plugin, and the
  // caller falls back to the channel count alone.

  static const struct
  {
    AudioChannelLabel label;
    uint8_t clap;
  } kLabels[] = {
      {kAudioChannelLabel_Left,                 CLAP_SURROUND_FL},
      {kAudioChannelLabel_Right,                CLAP_SURROUND_FR},
      {kAudioChannelLabel_Center,               CLAP_SURROUND_FC},
      {kAudioChannelLabel_LFEScreen,            CLAP_SURROUND_LFE},
      {kAudioChannelLabel_RearSurroundLeft,     CLAP_SURROUND_BL},
      {kAudioChannelLabel_RearSurroundRight,    CLAP_SURROUND_BR},
      {kAudioChannelLabel_LeftSurroundDirect,   CLAP_SURROUND_SL},
      {kAudioChannelLabel_RightSurroundDirect,  CLAP_SURROUND_SR},
      {kAudioChannelLabel_LeftCenter,           CLAP_SURROUND_FLC},
      {kAudioChannelLabel_RightCenter,          CLAP_SURROUND_FRC},
      {kAudioChannelLabel_CenterSurround,       CLAP_SURROUND_BC},
      {kAudioChannelLabel_TopCenterSurround,    CLAP_SURROUND_TC},
      {kAudioChannelLabel_VerticalHeightLeft,   CLAP_SURROUND_TFL},
      {kAudioChannelLabel_VerticalHeightCenter, CLAP_SURROUND_TFC},
      {kAudioChannelLabel_VerticalHeightRight,  CLAP_SURROUND_TFR},
      {kAudioChannelLabel_LeftTopRear,          CLAP_SURROUND_TBL},
      {kAudioChannelLabel_CenterTopRear,        CLAP_SURROUND_TBC},
      {kAudioChannelLabel_RightTopRear,         CLAP_SURROUND_TBR},
      {kAudioChannelLabel_TopBackLeft,          CLAP_SURROUND_TBL},
      {kAudioChannelLabel_TopBackCenter,        CLAP_SURROUND_TBC},
      {kAudioChannelLabel_TopBackRight,         CLAP_SURROUND_TBR},
      {kAudioChannelLabel_LeftTopMiddle,        CLAP_SURROUND_TSL},
      {kAudioChannelLabel_RightTopMiddle,       CLAP_SURROUND_TSR},
  };

// clang-format on

// The labels of a layout in channel order, whatever form the layout is in: a tag and a
// bitmap are expanded to the channel descriptions they stand for.
static bool labelsOf(const AudioChannelLayout *layout, std::vector<AudioChannelLabel> &labels)
{
  if (!layout)
  {
    return false;
  }

  const AudioChannelLayout *described = layout;
  std::vector<uint8_t> storage;

  if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions)
  {
    AudioFormatPropertyID property;
    UInt32 specifierSize;
    const void *specifier;
    AudioChannelLayoutTag tag = layout->mChannelLayoutTag;
    AudioChannelBitmap bitmap = layout->mChannelBitmap;

    if (tag == kAudioChannelLayoutTag_UseChannelBitmap)
    {
      property = kAudioFormatProperty_ChannelLayoutForBitmap;
      specifierSize = sizeof(bitmap);
      specifier = &bitmap;
    }
    else
    {
      property = kAudioFormatProperty_ChannelLayoutForTag;
      specifierSize = sizeof(tag);
      specifier = &tag;
    }

    UInt32 size = 0;
    if (AudioFormatGetPropertyInfo(property, specifierSize, specifier, &size) != noErr ||
        size < offsetof(AudioChannelLayout, mChannelDescriptions))
    {
      return false;
    }
    storage.resize(size);
    if (AudioFormatGetProperty(property, specifierSize, specifier, &size, storage.data()) != noErr)
    {
      return false;
    }
    described = reinterpret_cast<const AudioChannelLayout *>(storage.data());
  }

  labels.clear();
  for (UInt32 i = 0; i < described->mNumberChannelDescriptions; ++i)
  {
    labels.push_back(described->mChannelDescriptions[i].mChannelLabel);
  }
  return true;
}

static bool contains(const std::vector<AudioChannelLabel> &labels, AudioChannelLabel label)
{
  for (auto l : labels)
  {
    if (l == label) return true;
  }
  return false;
}

static bool contains(const uint8_t *map, uint32_t count, uint8_t position)
{
  for (uint32_t i = 0; i < count; ++i)
  {
    if (map[i] == position) return true;
  }
  return false;
}

bool channelMapFromLayout(const AudioChannelLayout *layout, uint32_t channelCount,
                          std::vector<uint8_t> &map)
{
  std::vector<AudioChannelLabel> labels;
  if (!labelsOf(layout, labels) || labels.size() != channelCount || channelCount == 0)
  {
    return false;
  }

  const bool hasRearPair = contains(labels, kAudioChannelLabel_RearSurroundLeft) ||
                           contains(labels, kAudioChannelLabel_RearSurroundRight);

  std::vector<uint8_t> result;
  result.reserve(channelCount);
  for (auto label : labels)
  {
    bool found = false;
    uint8_t position = 0;

    if (label == kAudioChannelLabel_LeftSurround)
    {
      position = hasRearPair ? CLAP_SURROUND_SL : CLAP_SURROUND_BL;
      found = true;
    }
    else if (label == kAudioChannelLabel_RightSurround)
    {
      position = hasRearPair ? CLAP_SURROUND_SR : CLAP_SURROUND_BR;
      found = true;
    }
    else
    {
      for (const auto &entry : kLabels)
      {
        if (entry.label == label)
        {
          position = entry.clap;
          found = true;
          break;
        }
      }
    }

    // a channel CLAP cannot place, or two channels on the same position
    if (!found || contains(result.data(), static_cast<uint32_t>(result.size()), position))
    {
      return false;
    }
    result.push_back(position);
  }

  map = std::move(result);
  return true;
}

bool ambisonicConfigFromLayout(const AudioChannelLayout *layout, uint32_t channelCount,
                               clap_ambisonic_config_t &config)
{
  if (!layout || channelCount == 0)
  {
    return false;
  }

  const AudioChannelLayoutTag tag = layout->mChannelLayoutTag;
  if (AudioChannelLayoutTag_GetNumberOfChannels(tag) != channelCount)
  {
    return false;
  }

  switch (tag & 0xFFFF0000)
  {
    case kAudioChannelLayoutTag_HOA_ACN_SN3D:
      config.ordering = CLAP_AMBISONIC_ORDERING_ACN;
      config.normalization = CLAP_AMBISONIC_NORMALIZATION_SN3D;
      return true;
    case kAudioChannelLayoutTag_HOA_ACN_N3D:
      config.ordering = CLAP_AMBISONIC_ORDERING_ACN;
      config.normalization = CLAP_AMBISONIC_NORMALIZATION_N3D;
      return true;
    default:
      break;
  }

  if (tag == kAudioChannelLayoutTag_Ambisonic_B_Format)
  {
    // W X Y Z, the classic first order B-Format
    config.ordering = CLAP_AMBISONIC_ORDERING_FUMA;
    config.normalization = CLAP_AMBISONIC_NORMALIZATION_MAXN;
    return true;
  }

  return false;
}

static AudioChannelLabel labelOf(uint8_t position, const uint8_t *map, uint32_t count)
{
  const bool hasSidePair =
      contains(map, count, CLAP_SURROUND_SL) || contains(map, count, CLAP_SURROUND_SR);

  switch (position)
  {
    case CLAP_SURROUND_SL:
      return kAudioChannelLabel_LeftSurround;
    case CLAP_SURROUND_SR:
      return kAudioChannelLabel_RightSurround;
    case CLAP_SURROUND_BL:
      return hasSidePair ? kAudioChannelLabel_RearSurroundLeft : kAudioChannelLabel_LeftSurround;
    case CLAP_SURROUND_BR:
      return hasSidePair ? kAudioChannelLabel_RearSurroundRight : kAudioChannelLabel_RightSurround;
    default:
      break;
  }

  for (const auto &entry : kLabels)
  {
    if (entry.clap == position)
    {
      return entry.label;
    }
  }
  return kAudioChannelLabel_Unknown;
}

AVAudioChannelLayout *layoutFromChannelMap(const uint8_t *map, uint32_t count)
{
  if (!map || count == 0)
  {
    return nil;
  }

  const size_t size =
      offsetof(AudioChannelLayout, mChannelDescriptions) + count * sizeof(AudioChannelDescription);
  std::vector<uint8_t> storage(size, 0);
  auto *layout = reinterpret_cast<AudioChannelLayout *>(storage.data());
  layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
  layout->mNumberChannelDescriptions = count;

  std::vector<AudioChannelLabel> labels;
  for (uint32_t i = 0; i < count; ++i)
  {
    auto label = labelOf(map[i], map, count);
    if (label == kAudioChannelLabel_Unknown || contains(labels, label))
    {
      return nil;
    }
    labels.push_back(label);
    layout->mChannelDescriptions[i].mChannelLabel = label;
  }

  // Hosts show and offer layouts by their tag, so use one where CoreAudio has a tag for
  // exactly these channels - in exactly this order, which is checked rather than trusted:
  // the channel order is what the plugin is handed the audio in.
  AudioChannelLayoutTag tag = 0;
  UInt32 tagSize = sizeof(tag);
  if (AudioFormatGetProperty(kAudioFormatProperty_TagForChannelLayout, static_cast<UInt32>(size), layout,
                             &tagSize, &tag) == noErr &&
      tag != kAudioChannelLayoutTag_UseChannelDescriptions)
  {
    AudioChannelLayout tagged{};
    tagged.mChannelLayoutTag = tag;
    std::vector<AudioChannelLabel> tagLabels;
    if (labelsOf(&tagged, tagLabels) && tagLabels == labels)
    {
      return [AVAudioChannelLayout layoutWithLayoutTag:tag];
    }
  }

  return [[AVAudioChannelLayout alloc] initWithLayout:layout];
}

AVAudioChannelLayout *layoutFromAmbisonicConfig(const clap_ambisonic_config_t &config,
                                                uint32_t channelCount)
{
  // (order + 1)^2 channels, which every HOA tag requires
  uint32_t order = 0;
  while ((order + 1) * (order + 1) < channelCount) ++order;
  if (channelCount == 0 || (order + 1) * (order + 1) != channelCount)
  {
    return nil;
  }

  if (config.ordering == CLAP_AMBISONIC_ORDERING_ACN)
  {
    if (config.normalization == CLAP_AMBISONIC_NORMALIZATION_SN3D)
    {
      return
          [AVAudioChannelLayout layoutWithLayoutTag:kAudioChannelLayoutTag_HOA_ACN_SN3D | channelCount];
    }
    if (config.normalization == CLAP_AMBISONIC_NORMALIZATION_N3D)
    {
      return
          [AVAudioChannelLayout layoutWithLayoutTag:kAudioChannelLayoutTag_HOA_ACN_N3D | channelCount];
    }
  }
  else if (config.ordering == CLAP_AMBISONIC_ORDERING_FUMA &&
           config.normalization == CLAP_AMBISONIC_NORMALIZATION_MAXN && channelCount == 4)
  {
    return [AVAudioChannelLayout layoutWithLayoutTag:kAudioChannelLayoutTag_Ambisonic_B_Format];
  }

  // SN2D, N2D, FuMa above first order: CoreAudio has no tag for it
  return nil;
}

AVAudioChannelLayout *layoutFromChannelCount(uint32_t channelCount)
{
  // in the WAVE channel order, which is the order CLAP's positions are numbered in
  static const uint8_t k30[] = {CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC};
  static const uint8_t kQuad[] = {CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_BL,
                                  CLAP_SURROUND_BR};
  static const uint8_t k50[] = {CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC, CLAP_SURROUND_BL,
                                CLAP_SURROUND_BR};
  static const uint8_t k51[] = {CLAP_SURROUND_FL,  CLAP_SURROUND_FR, CLAP_SURROUND_FC,
                                CLAP_SURROUND_LFE, CLAP_SURROUND_BL, CLAP_SURROUND_BR};
  static const uint8_t k70[] = {CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC, CLAP_SURROUND_BL,
                                CLAP_SURROUND_BR, CLAP_SURROUND_SL, CLAP_SURROUND_SR};
  static const uint8_t k71[] = {CLAP_SURROUND_FL, CLAP_SURROUND_FR, CLAP_SURROUND_FC, CLAP_SURROUND_LFE,
                                CLAP_SURROUND_BL, CLAP_SURROUND_BR, CLAP_SURROUND_SL, CLAP_SURROUND_SR};

  AVAudioChannelLayout *layout = nil;
  switch (channelCount)
  {
    case 0:
      return nil;
    case 1:
      return [AVAudioChannelLayout layoutWithLayoutTag:kAudioChannelLayoutTag_Mono];
    case 2:
      return [AVAudioChannelLayout layoutWithLayoutTag:kAudioChannelLayoutTag_Stereo];
    case 3:
      layout = layoutFromChannelMap(k30, 3);
      break;
    case 4:
      layout = layoutFromChannelMap(kQuad, 4);
      break;
    case 5:
      layout = layoutFromChannelMap(k50, 5);
      break;
    case 6:
      layout = layoutFromChannelMap(k51, 6);
      break;
    case 7:
      layout = layoutFromChannelMap(k70, 7);
      break;
    case 8:
      layout = layoutFromChannelMap(k71, 8);
      break;
    default:
      break;
  }

  if (!layout)
  {
    layout =
        [AVAudioChannelLayout layoutWithLayoutTag:kAudioChannelLayoutTag_DiscreteInOrder | channelCount];
  }
  return layout;
}
}  // namespace Clap::AUv3Channels

#pragma clang diagnostic pop
