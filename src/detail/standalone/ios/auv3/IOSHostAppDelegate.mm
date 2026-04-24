/*
    IOSHostAppDelegate.mm

    The iOS counterpart to macOS's AUv3HostAppDelegate. Discovers the
    embedded AUv3 extension via AudioComponentDescription, instantiates
    the AU, asks it for its request view controller, and makes that VC
    the root of the host app's window.

    The component description (type / subtype / manufacturer) is baked
    in at compile time from -DAU_TYPE_STR=... etc. set by
    target_add_auv3_standalone_ios_wrapper. This lets a single host
    source file serve any hosted CLAP without needing the XIB/Storyboard
    that the macOS version uses.
*/

#import "IOSHostAppDelegate.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <AVFoundation/AVFoundation.h>

// FourCC conversion — compile-time codes are passed as 4-char strings so
// the CMake layer doesn't have to pack them into uint32 literals.
static OSType FourCCFromString(const char *s)
{
    if (!s || strlen(s) < 4) return 0;
    return ((OSType)(unsigned char)s[0] << 24) | ((OSType)(unsigned char)s[1] << 16)
         | ((OSType)(unsigned char)s[2] << 8)  |  (OSType)(unsigned char)s[3];
}

#ifndef AU_TYPE_STR
#define AU_TYPE_STR "aumu"
#endif
#ifndef AU_SUBTYPE_STR
#define AU_SUBTYPE_STR "none"
#endif
#ifndef AU_MANUFACTURER_STR
#define AU_MANUFACTURER_STR "none"
#endif

@interface IOSHostViewController : UIViewController
@property(nonatomic, strong) AUAudioUnit *audioUnit;
@property(nonatomic, strong) UIViewController *pluginViewController;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) UIScrollView *statusScrollView;
@property(nonatomic, strong) AVAudioEngine *audioEngine;
@property(nonatomic, assign) BOOL didRegisterCatalogObserver;
@property(nonatomic, assign) BOOL sessionConfigured;
@end

// Render a FourCC uint32 as a 4-char NSString for display. Non-printable
// bytes get escaped so we don't produce broken UTF-8 in the status label.
static NSString *FourCCToString(OSType code)
{
    unsigned char b[4] = {
        (unsigned char)((code >> 24) & 0xff),
        (unsigned char)((code >> 16) & 0xff),
        (unsigned char)((code >>  8) & 0xff),
        (unsigned char)( code        & 0xff)
    };
    char buf[16];
    int o = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (b[i] >= 0x20 && b[i] < 0x7f) buf[o++] = (char)b[i];
        else o += snprintf(buf + o, sizeof(buf) - o, "\\x%02x", b[i]);
    }
    buf[o] = 0;
    return [NSString stringWithUTF8String:buf];
}

@implementation IOSHostViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    // Scrollable status so the full AU-catalog dump in the failure case
    // fits — the list of third-party AUs can be dozens of lines.
    self.statusScrollView = [[UIScrollView alloc] init];
    self.statusScrollView.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.statusScrollView];

    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.text = @"Loading AUv3…";
    self.statusLabel.textAlignment = NSTextAlignmentLeft;
    self.statusLabel.numberOfLines = 0;
    self.statusLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    [self.statusScrollView addSubview:self.statusLabel];
    [NSLayoutConstraint activateConstraints:@[
        [self.statusScrollView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [self.statusScrollView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [self.statusScrollView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:20],
        [self.statusScrollView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-20],

        [self.statusLabel.topAnchor constraintEqualToAnchor:self.statusScrollView.topAnchor constant:12],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.statusScrollView.leadingAnchor],
        [self.statusLabel.trailingAnchor constraintEqualToAnchor:self.statusScrollView.trailingAnchor],
        [self.statusLabel.widthAnchor constraintEqualToAnchor:self.statusScrollView.widthAnchor],
        [self.statusLabel.bottomAnchor constraintEqualToAnchor:self.statusScrollView.bottomAnchor constant:-12],
    ]];

    [self instantiateAU];
}

- (void)instantiateAU
{
    AudioComponentDescription desc = {0};
    desc.componentType         = FourCCFromString(AU_TYPE_STR);
    desc.componentSubType      = FourCCFromString(AU_SUBTYPE_STR);
    desc.componentManufacturer = FourCCFromString(AU_MANUFACTURER_STR);
    desc.componentFlags        = 0;
    desc.componentFlagsMask    = 0;

    // Configure + activate the audio session once per process.
    //
    // Activation is required on iOS 17+ before AVAudioUnitComponentManager
    // will return third-party AUv3 extensions to this process — setCategory
    // alone is not enough. Without setActive:YES our componentsMatching
    // query returns only the 40 built-in Apple system AUs from
    // AudioToolbox.framework, as if no third-party extensions exist.
    //
    // We only do this once; setCategory on every retry spams audiomxd and
    // interacts poorly with the registration-change notification.
    if (!self.sessionConfigured)
    {
        AVAudioSession *session = [AVAudioSession sharedInstance];
        NSError *sessionErr = nil;
        [session setCategory:AVAudioSessionCategoryPlayback error:&sessionErr];
        if (sessionErr) NSLog(@"AVAudioSession setCategory: %@", sessionErr);
        sessionErr = nil;
        [session setActive:YES error:&sessionErr];
        if (sessionErr) NSLog(@"AVAudioSession setActive: %@", sessionErr);

        // Construct + start an AVAudioEngine before querying AudioComponents.
        // On iOS the process-local AudioComponent catalog only includes
        // third-party AUv3 extensions once the process has stood up real
        // audio I/O — setCategory/setActive alone is not enough. Without
        // this step both componentsMatchingDescription: and the lower-level
        // AudioComponentFindNext return only Apple's built-in 40 AUs and
        // skip everything installed by other apps (observed via GarageBand
        // seeing the same catalog fine from the same device).
        //
        // Touching mainMixerNode auto-attaches it and auto-connects it to
        // outputNode, producing a minimal valid graph. startAndReturnError:
        // silently refuses to start an empty graph.
        self.audioEngine = [[AVAudioEngine alloc] init];
        (void)self.audioEngine.mainMixerNode;
        NSError *engineErr = nil;
        if (![self.audioEngine startAndReturnError:&engineErr])
            NSLog(@"AVAudioEngine start: %@", engineErr);

        self.sessionConfigured = YES;
    }

    // Touch AVAudioUnitComponentManager once to prime the process-local
    // AU registry from the system + embedded .appex AudioComponents.
    // Without this, instantiateWithComponentDescription can return
    // paramErr (-50) on iOS simulator because the registry isn't hot yet.
    AVAudioUnitComponentManager *mgr = [AVAudioUnitComponentManager sharedAudioUnitComponentManager];
    AudioComponentDescription wildcard = {0};
    NSArray<AVAudioUnitComponent *> *allComponents =
        [mgr componentsMatchingDescription:wildcard];

    // Cross-check via the lower-level C API. AudioComponentFindNext hits
    // the same registry but without AVAudioUnitComponentManager's extra
    // sandbox-aware filter, so if third-party AUs are present but hidden
    // by the filter, we'll see them here. On macOS this is in fact what
    // AUv3HostAppDelegate uses as its primary discovery path.
    int lowLevelMatches = 0;
    AudioComponent lowLevelFound = AudioComponentFindNext(NULL, &desc);
    if (lowLevelFound) lowLevelMatches = 1;
    int lowLevelThirdParty = 0;
    {
        AudioComponent c = NULL;
        AudioComponentDescription any = {0};
        while ((c = AudioComponentFindNext(c, &any)) != NULL)
        {
            AudioComponentDescription cd = {0};
            AudioComponentGetDescription(c, &cd);
            if (cd.componentManufacturer != 'appl') ++lowLevelThirdParty;
        }
    }

    // Register catalog-changed observer exactly once. audiomxd may finish
    // discovering our extension after the host's first query completed;
    // re-querying on each registration change gives us a second chance.
    if (!self.didRegisterCatalogObserver)
    {
        __weak IOSHostViewController *weakSelf = self;
        [[NSNotificationCenter defaultCenter]
            addObserverForName:AVAudioUnitComponentManagerRegistrationsChangedNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *note) {
            __strong IOSHostViewController *strongSelf = weakSelf;
            if (!strongSelf || strongSelf.audioUnit) return;
            [strongSelf instantiateAU];
        }];
        self.didRegisterCatalogObserver = YES;
    }

    __weak IOSHostViewController *weakSelf = self;
    [AUAudioUnit instantiateWithComponentDescription:desc
                                             options:0
                                   completionHandler:^(AUAudioUnit * _Nullable au,
                                                       NSError * _Nullable error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            __strong IOSHostViewController *strongSelf = weakSelf;
            if (!strongSelf) return;
            if (error || !au)
            {
                NSArray<AVAudioUnitComponent *> *cands = [mgr componentsMatchingDescription:desc];

                // Dump every visible component so we can tell whether our AU
                // is in the catalog at all, or in the catalog but registered
                // under unexpected codes (stale install, version mismatch).
                // Apple/system AUs go at the bottom since they are never the
                // source of the mismatch we're debugging.
                NSMutableArray<NSString *> *thirdParty = [NSMutableArray array];
                NSMutableArray<NSString *> *system = [NSMutableArray array];
                for (AVAudioUnitComponent *c in allComponents)
                {
                    AudioComponentDescription cd = c.audioComponentDescription;
                    NSString *line = [NSString stringWithFormat:@"  %@/%@/%@  %@",
                        FourCCToString(cd.componentType),
                        FourCCToString(cd.componentSubType),
                        FourCCToString(cd.componentManufacturer),
                        c.name];
                    NSString *mfr = c.manufacturerName ?: @"";
                    if ([mfr hasPrefix:@"Apple"] || mfr.length == 0)
                        [system addObject:line];
                    else
                        [thirdParty addObject:line];
                }

                NSMutableString *msg = [NSMutableString string];
                [msg appendFormat:@"Failed to instantiate AUv3:\n%@\n\n",
                    error ? error.localizedDescription : @"unknown error"];
                [msg appendFormat:@"iOS: %@  Device: %@\n",
                    UIDevice.currentDevice.systemVersion,
                    UIDevice.currentDevice.model];
                [msg appendFormat:@"Looking for: %s / %s / %s\n",
                    AU_TYPE_STR, AU_SUBTYPE_STR, AU_MANUFACTURER_STR];
                [msg appendFormat:@"Matches in registry: %lu\n",
                    (unsigned long)cands.count];
                [msg appendFormat:@"Total AUs visible: %lu\n",
                    (unsigned long)allComponents.count];
                [msg appendFormat:@"AudioComponentFindNext exact match: %d\n",
                    lowLevelMatches];
                [msg appendFormat:@"AudioComponentFindNext 3rd-party total: %d\n\n",
                    lowLevelThirdParty];
                [msg appendFormat:@"Third-party (%lu):\n%@\n",
                    (unsigned long)thirdParty.count,
                    thirdParty.count ? [thirdParty componentsJoinedByString:@"\n"]
                                     : @"  (none)"];
                [msg appendFormat:@"\nSystem (%lu):\n%@",
                    (unsigned long)system.count,
                    [system componentsJoinedByString:@"\n"]];
                strongSelf.statusLabel.text = msg;
                return;
            }
            strongSelf.audioUnit = au;
            [strongSelf requestPluginVC];
        });
    }];
}


- (void)requestPluginVC
{
    __weak IOSHostViewController *weakSelf = self;
    [self.audioUnit requestViewControllerWithCompletionHandler:^(UIViewController * _Nullable vc) {
        dispatch_async(dispatch_get_main_queue(), ^{
            __strong IOSHostViewController *strongSelf = weakSelf;
            if (!strongSelf) return;
            if (!vc)
            {
                strongSelf.statusLabel.text = @"AU instantiated but has no UI view controller.";
                return;
            }
            [strongSelf embedPluginVC:vc];
        });
    }];
}

- (void)embedPluginVC:(UIViewController *)vc
{
    self.pluginViewController = vc;
    [self addChildViewController:vc];
    vc.view.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:vc.view];
    [NSLayoutConstraint activateConstraints:@[
        [vc.view.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [vc.view.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
        [vc.view.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [vc.view.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
    ]];
    [vc didMoveToParentViewController:self];
    self.statusScrollView.hidden = YES;
}

@end

@implementation IOSHostAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    self.window.rootViewController = [[IOSHostViewController alloc] init];
    [self.window makeKeyAndVisible];
    return YES;
}

@end
