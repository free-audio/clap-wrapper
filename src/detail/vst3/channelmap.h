#pragma once

/*
    VST3 speaker arrangements <-> CLAP channel maps

    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    A VST3 Vst::SpeakerArrangement is a bitset of speakers, and the channels of a bus are
    ordered by the bit position of their speaker. A CLAP channel map is a list of
    CLAP_SURROUND_* positions in the plugin's own channel order. These helpers translate
    between the two so the layout survives in both directions:

      - setBusArrangements() turns the arrangement the host asks for into a channel map and
        sends it to the plugin as a CLAP_PORT_SURROUND configuration request, so a plugin
        can tell 7.1 Cine from 7.1 Music from 5.1.2 and knows which channel is the LFE
      - addAudioBusFrom() turns the channel map of a CLAP_PORT_SURROUND port back into an
        arrangement, so a plugin that implements clap.surround states its own layout

    Ambisonics are covered too, as far as VST3 allows: it only knows ACN ordered
    arrangements up to 3rd order and says nothing about normalization.
*/

#include <cstdint>
#include <vector>

#include <clap/clap.h>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <pluginterfaces/vst/vstspeaker.h>

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace Clap::Vst3Channels
{
// Builds the CLAP channel map of an arrangement, in that arrangement's VST3 channel order.
// Returns false - leaving map untouched - when the arrangement holds a speaker CLAP has no
// position for, in which case no surround request can be made for it.
bool channelMapFromArrangement(Steinberg::Vst::SpeakerArrangement arr, std::vector<uint8_t> &map);

// Builds the arrangement a CLAP channel map describes. Returns kEmpty when the map has no
// VST3 equivalent: an unknown position, the same position twice, or - because the channels
// of a VST3 bus always run in ascending speaker order - a map in any other order.
Steinberg::Vst::SpeakerArrangement arrangementFromChannelMap(const uint8_t *map, uint32_t count);

// The ambisonic configuration an arrangement describes, for CLAP_PORT_AMBISONIC requests.
// Returns false when the arrangement is not an ambisonic one.
bool ambisonicConfigFromArrangement(Steinberg::Vst::SpeakerArrangement arr,
                                    clap_ambisonic_config_t &config);

// The arrangement for an ambisonic port of the given channel count. kEmpty when VST3 has no
// equivalent, which is every ordering but ACN and every order above the 3rd.
Steinberg::Vst::SpeakerArrangement arrangementFromAmbisonicConfig(const clap_ambisonic_config_t &config,
                                                                  uint32_t channel_count);
}  // namespace Clap::Vst3Channels
