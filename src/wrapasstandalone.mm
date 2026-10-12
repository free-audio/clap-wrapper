#import <Cocoa/Cocoa.h>
#import "detail/standalone/macos/AppDelegate.h"

int main(int argc, const char *argv[])
{
  @autoreleasepool
  {
    // NSApp.delegate is weak
    static ClapWrapperAppDelegate *delegate = [[ClapWrapperAppDelegate alloc] init];

    auto *app = NSApplication.sharedApplication;
    app.delegate = delegate;
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    [app run];
  }
  return 0;
}
