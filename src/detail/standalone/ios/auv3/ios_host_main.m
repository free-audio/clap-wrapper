/*
    ios_host_main.m — entry point for the iOS AUv3 host app.

    A minimal UIApplicationMain shim. The real app logic lives in
    IOSHostAppDelegate.
*/

#import <UIKit/UIKit.h>
#import "IOSHostAppDelegate.h"

int main(int argc, char *argv[])
{
  @autoreleasepool
  {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([IOSHostAppDelegate class]));
  }
}
