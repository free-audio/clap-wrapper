#pragma once

/*
    AUv3 Factory

    Copyright (c) 2024 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.
*/

#import <AudioToolbox/AudioToolbox.h>

@interface ClapAUv3Factory : NSObject <AUAudioUnitFactory>
- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error;
@end
