#pragma once

/*
 * This is the set of wedge classes which the auv2 code generator projects onto. It is very
 * unlikely you would need to edit these classes since they almost entirely delegate to the
 * base class `ClapAUv2_Base` which uses CRTP to have the appropriate AU base class.
 */

#include <AudioUnitSDK/AUEffectBase.h>
#include <AudioUnitSDK/AUMIDIEffectBase.h>
#include <AudioUnitSDK/MusicDeviceBase.h>

#include <iostream>
#include "auv2_shared.h"
#include <memory>

namespace free_audio::auv2_wrapper
{

// -------------------------------------------------------------------------------------------------

class ClapWrapper_AUV2_Effect : public ClapAUv2_Base<ausdk::AUEffectBase, false, true, false>
{
  using Base = ClapAUv2_Base<ausdk::AUEffectBase, false, true, false>;

 public:
  explicit ClapWrapper_AUV2_Effect(const std::string &clapname, const std::string &clapid, int clapidx,
                                   AudioComponentInstance ci)
    : Base{clapname, clapid, clapidx, ci}
  {
  }
};

class ClapWrapper_AUV2_NoteEffect : public ClapAUv2_Base<ausdk::AUMIDIEffectBase, false, false, true>
{
  using Base = ClapAUv2_Base<ausdk::AUMIDIEffectBase, false, false, true>;

 public:
  explicit ClapWrapper_AUV2_NoteEffect(const std::string &clapname, const std::string &clapid,
                                       int clapidx, AudioComponentInstance ci)
    : Base{clapname, clapid, clapidx, ci}
  {
  }
};

// -------------------------------------------------------------------------------------------------

class ClapWrapper_AUV2_Instrument : public ClapAUv2_Base<ausdk::MusicDeviceBase, true, false, false>
{
  using Base = ClapAUv2_Base<ausdk::MusicDeviceBase, true, false, false>;

 public:
  explicit ClapWrapper_AUV2_Instrument(const std::string &clapname, const std::string &clapid,
                                       int clapidx, AudioComponentInstance ci)
    : Base{clapname, clapid, clapidx, ci, 0, 1}
  {
  }
};
}  // namespace free_audio::auv2_wrapper
