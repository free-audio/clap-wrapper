/*
    wrapasauv3standalone.mm

    Copyright (c) 2024 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    Entry point for the AUv3 standalone host application.
*/

#import <Cocoa/Cocoa.h>
#import "AUv3HostAppDelegate.h"

int main(int argc, const char *argv[])
{
  @autoreleasepool
  {
    // NSApp.delegate is weak
    static AUv3HostAppDelegate *delegate = [[AUv3HostAppDelegate alloc] init];

    auto *app = NSApplication.sharedApplication;
    app.delegate = delegate;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    [app run];
  }
  return 0;
}
