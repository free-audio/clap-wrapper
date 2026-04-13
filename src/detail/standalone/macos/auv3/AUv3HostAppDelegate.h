#pragma once

#import <Cocoa/Cocoa.h>

@interface AUv3HostAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>

@property(nonatomic, weak) IBOutlet NSWindow *window;

@end
