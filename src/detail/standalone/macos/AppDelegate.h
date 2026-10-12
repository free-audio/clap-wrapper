#import <Cocoa/Cocoa.h>

@interface ClapWrapperAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>

@property(strong) NSTimer *requestCallbackTimer;
@property(strong) NSWindow *window;

- (IBAction)openAudioSettingsWindow:(id)sender;

- (IBAction)streamWrapperFile:(id)sender;
- (IBAction)streamWrapperFileAs:(id)sender;
- (IBAction)openWrapperFile:(id)sender;

@end
