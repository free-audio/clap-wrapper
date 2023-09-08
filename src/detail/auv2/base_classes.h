/*
 * document
 */

#pragma once

#include <AudioUnitSDK/AUEffectBase.h>
#include <AudioUnitSDK/AUMIDIEffectBase.h>
#include <AudioUnitSDK/MusicDeviceBase.h>

#include <iostream>

// -------------------------------------------------------------------------------------------------

class ClapWrapper_AUV2_Effect : public ausdk::AUEffectBase
{
    using Base = ausdk::AUEffectBase;

  public:
    explicit ClapWrapper_AUV2_Effect(const std::string &clapid, int clapidx,
                                     AudioComponentInstance ci)
        : Base{ci, true}
    {
        std::cout << "[clap-wrapper] auv2: creating effect" << std::endl;
        std::cout << "[clap-wrapper] auv2: id='" << clapid << "' index=" << clapidx << std::endl;
    }
};

class ClapWrapper_AUV2_NoteEffect : public ausdk::AUMIDIEffectBase
{
    using Base = ausdk::AUMIDIEffectBase;

  public:
    explicit ClapWrapper_AUV2_NoteEffect(const std::string &clapid, int clapidx,
                                         AudioComponentInstance ci)
        : Base{ci, true}
    {
        std::cout << "[clap-wrapper] auv2: creating note effect" << std::endl;
        std::cout << "[clap-wrapper] auv2: id='" << clapid << "' index=" << clapidx << std::endl;
    }
};

// -------------------------------------------------------------------------------------------------

class ClapWrapper_AUV2_Instrument : public ausdk::MusicDeviceBase
{
    using Base = ausdk::MusicDeviceBase;

  public:
    explicit ClapWrapper_AUV2_Instrument(const std::string &clapid, int clapidx,
                                         AudioComponentInstance ci)
        : Base{ci, 0, 1}
    {
        std::cout << "[clap-wrapper] auv2: creating instrument" << std::endl;
        std::cout << "[clap-wrapper] auv2: id='" << clapid << "' index=" << clapidx << std::endl;
    }

    bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override { return true; }

    bool CanScheduleParameters() const override { return false; }
};
