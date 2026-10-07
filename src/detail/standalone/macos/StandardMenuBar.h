#ifndef CLAP_WRAPPER_STANDALONE_MACOS_STANDARDMENUBAR_H
#define CLAP_WRAPPER_STANDALONE_MACOS_STANDARDMENUBAR_H

#import <Cocoa/Cocoa.h>

namespace freeaudio::clap_wrapper::standalone::macos
{
// call from applicationWillFinishLaunching:, returns the File menu for extra items
NSMenu *installStandardMenuBar(SEL settingsAction, NSString *settingsTitle);
}  // namespace freeaudio::clap_wrapper::standalone::macos

#endif
