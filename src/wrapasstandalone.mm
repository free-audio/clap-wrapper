#import <Cocoa/Cocoa.h>

#include "detail/standalone/entry.h"

int main(int argc, const char *argv[])
{
  if (freeaudio::clap_wrapper::standalone::mainListAudioDevices(argc, argv)) return 0;

  return NSApplicationMain(argc, argv);
}
