/*
 * document
 */

#pragma once

#include <AudioUnitSDK/AUEffectBase.h>
#include <AudioUnitSDK/AUMIDIEffectBase.h>
#include <AudioUnitSDK/MusicDeviceBase.h>

#include <iostream>
#include "auv2_shared.h"
#include <memory>

namespace free_audio::auv2_wrapper
{

  // -------------------------------------------------------------------------------------------------

  class ClapWrapper_AUV2_Effect : public ausdk::AUEffectBase
  {
    using Base = ausdk::AUEffectBase;
    ClapBridge bridge;

   public:
    explicit ClapWrapper_AUV2_Effect(const std::string &clapname, const std::string &clapid, int clapidx,
                                     AudioComponentInstance ci)
        : Base{ci, true}, bridge(clapname, clapid, clapidx)
    {
      std::cout << "[clap-wrapper] auv2: creating effect" << std::endl;
      std::cout << "[clap-wrapper] auv2: id='" << clapid << "' index=" << clapidx << std::endl;

      bridge.initialize();
    }
  };

  class ClapWrapper_AUV2_NoteEffect : public ausdk::AUMIDIEffectBase
  {
    using Base = ausdk::AUMIDIEffectBase;
    ClapBridge bridge;

   public:
    explicit ClapWrapper_AUV2_NoteEffect(const std::string &clapname, const std::string &clapid,
                                         int clapidx, AudioComponentInstance ci)
        : Base{ci, true}, bridge(clapname, clapid, clapidx)
    {
      std::cout << "[clap-wrapper] auv2: creating note effect" << std::endl;
      std::cout << "[clap-wrapper] auv2: id='" << clapid << "' index=" << clapidx << std::endl;

      bridge.initialize();
    }
  };

  // -------------------------------------------------------------------------------------------------

  class ClapWrapper_AUV2_Instrument : public ausdk::MusicDeviceBase
  {
    using Base = ausdk::MusicDeviceBase;
    ClapBridge bridge;

   public:
    explicit ClapWrapper_AUV2_Instrument(const std::string &clapname, const std::string &clapid,
                                         int clapidx, AudioComponentInstance ci)
        : Base{ci, 0, 1}, bridge(clapname, clapid, clapidx)
    {
      std::cout << "[clap-wrapper] auv2: creating instrument" << std::endl;
      std::cout << "[clap-wrapper] auv2: id='" << clapid << "' index=" << clapidx << std::endl;

      bridge.initialize();
    }

    bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override
    {
      return true;
    }

    bool CanScheduleParameters() const override
    {
      return false;
    }
  };
}  // namespace free_audio::auv2_wrapper