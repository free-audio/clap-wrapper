#import <Cocoa/Cocoa.h>

// @class AudioSettingsWindowDelegate;

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
{
  // AudioSettingsWindowDelegate *audioSettingsWindowDelegate;
}
@property(assign) IBOutlet NSWindow *window;

- (IBAction)openAudioSettingsWindow:(id)sender;

@end
