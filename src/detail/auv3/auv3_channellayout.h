#pragma once

/*
    AUv3 channel layouts <-> CLAP channel maps

    Copyright (c) 2026 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    An AUAudioUnitBus carries its layout in the AVAudioChannelLayout of its format: a
    CoreAudio layout tag, or a list of channel labels in channel order. A CLAP port
    describes the same thing as a list of CLAP_SURROUND_* positions (clap.surround) or an
    ambisonic configuration (clap.ambisonic). These helpers translate between the two, the
    AUv3 counterpart of detail/vst3/channelmap.h:

      - the layout the host sets on a bus becomes a CLAP_PORT_SURROUND or
        CLAP_PORT_AMBISONIC configuration request, so a plugin can tell 7.1 from 5.1.2 and
        knows which channel is the LFE
      - the channel map of a port becomes the layout of its bus, and with it the layout
        tag the host shows and offers

    Any bus with more than two channels needs a layout: AVAudioFormat refuses to describe
    one without.
*/

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <clap/clap.h>

#include <cstdint>
#include <vector>

namespace Clap::AUv3Channels
{
// Builds the CLAP channel map of a layout, in the layout's channel order. Returns false -
// leaving map untouched - when the layout does not have channelCount channels or holds a
// channel CLAP has no position for (discrete channels among them).
bool channelMapFromLayout(const AudioChannelLayout *layout, uint32_t channelCount,
                          std::vector<uint8_t> &map);

// The ambisonic configuration a layout describes. Returns false for anything that is not
// one of the HOA or B-Format tags.
bool ambisonicConfigFromLayout(const AudioChannelLayout *layout, uint32_t channelCount,
                               clap_ambisonic_config_t &config);

// The layout a CLAP channel map describes - a layout tag where CoreAudio has one for
// exactly these channels in exactly this order, the channel labels otherwise. nil when the
// map holds a position twice or one CoreAudio has no label for.
AVAudioChannelLayout *layoutFromChannelMap(const uint8_t *map, uint32_t count);

// The layout of an ambisonic port. nil when CoreAudio has no tag for the configuration.
AVAudioChannelLayout *layoutFromAmbisonicConfig(const clap_ambisonic_config_t &config,
                                                uint32_t channelCount);

// The last resort, for a port that says nothing about its layout: guessed from the
// channel count, the same guesses the VST3 wrapper makes, discrete channels for a count
// that has no usual layout. nil for 0 channels.
AVAudioChannelLayout *layoutFromChannelCount(uint32_t channelCount);
}  // namespace Clap::AUv3Channels
