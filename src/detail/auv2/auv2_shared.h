#pragma once

/*
 * This is the base class of all our AU. It is templated on the AU base class and the configuration
 * type (instrument, effect, note effect).
 */

#include "detail/clap/fsutil.h"
#include <iostream>

#include "detail/os/osutil.h"
#include "clap_proxy.h"

#include <AudioToolbox/AudioUnitProperties.h>
#include <CoreMIDI/MIDIServices.h>
#define CWAUTRACE                                                                               \
  std::cout << "[clap-wrapper auv2 trace] " << __func__ << " @ auv2_shared.h line " << __LINE__ \
            << std::endl

static const AudioUnitPropertyID kAudioUnitProperty_ClapWrapper_UIConnection_id = 0x00010911;

namespace free_audio::auv2_wrapper
{

typedef struct ui_connection {
  uint32_t identifier = kAudioUnitProperty_ClapWrapper_UIConnection_id;
  Clap::Plugin* _plugin = nullptr;
  clap_window_t* _window = nullptr;
} ui_connection;


}  // namespace free_audio::auv2_wrapper

#undef CWAUTRACE
