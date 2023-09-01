//
//  EmptyPlugIns.cpp
//  EmptyPlugIns - this is just the empty plugin shell from the SDK for now
//

#include <AudioUnitSDK/MusicDeviceBase.h>

#if !defined(CLAP_AUSDK_BASE_CLASS)
#define CLAP_AUSDK_BASE_CLASS ausdk::MusicDeviceBase
#endif

struct wrapAsAUV2 : public CLAP_AUSDK_BASE_CLASS
{
    using Base = CLAP_AUSDK_BASE_CLASS;

    wrapAsAUV2(AudioComponentInstance ci) : Base{ci, 1, 1} {}
    bool StreamFormatWritable(AudioUnitScope, AudioUnitElement) override { return true; }
    bool CanScheduleParameters() const override { return false; }



};

AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, wrapAsAUV2)
