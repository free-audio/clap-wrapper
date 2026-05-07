/*
    IOSHostAppDelegate.mm

    The iOS counterpart to macOS's AUv3HostAppDelegate. Instantiates the
    embedded AUv3 plugin in-process and embeds its view controller as the
    root of the host app's window.

    Why not the usual AVAudioUnitComponentManager / AVAudioUnit
    instantiateWithComponentDescription: flow? On iOS 18+ the process-local
    AudioComponent catalog hides third-party AUv3 extensions from every
    host except GarageBand-class Apple-blessed ones — even for the host's
    own embedded .appex. componentsMatchingDescription and the lower-level
    AudioComponentFindNext both return 0 third-party matches. Instead we
    statically link the AUv3 wrapper runtime and its generated factory
    class into the host (via wrap_auv3_standalone_ios.cmake) and
    instantiate the factory as ObjC directly, bypassing the registry. The
    same .appex is still produced and still works in external hosts that
    don't hit the sandbox (GarageBand, AUM, etc.).

    AU identity + factory class name are baked in at compile time via
    -DAU_TYPE_STR=... and -DAUV3_FACTORY_CLASS_NAME_STR=... set by the
    CMake wrapper function.
*/

#import "IOSHostAppDelegate.h"
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreMIDI/CoreMIDI.h>

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
#ifndef AUV3_FACTORY_CLASS_NAME_STR
#define AUV3_FACTORY_CLASS_NAME_STR ""
#endif

@interface IOSHostViewController : UIViewController
@property(nonatomic, strong) AUAudioUnit *audioUnit;
@property(nonatomic, strong) UIViewController *pluginViewController;
@property(nonatomic, strong) UILabel *statusLabel;
@property(nonatomic, strong) AVAudioEngine *audioEngine;
@property(nonatomic, strong) AVAudioSourceNode *auSourceNode;
@property(nonatomic, assign) MIDIClientRef midiClient;
@property(nonatomic, assign) MIDIPortRef midiInputPort;
@property(nonatomic, copy)   AUScheduleMIDIEventBlock scheduleMIDIBlock;
@end

// CoreMIDI receive callback. Runs on a dedicated high-priority MIDI thread.
// We forward each short (status + up to 2 data bytes) packet directly to
// the AU's scheduleMIDIEventBlock, which is safe to call from any thread.
// SysEx and other long packets are dropped — the AU's short-event API
// won't accept them in a single call; wiring SysEx would require splitting
// across the block's length limit or using the newer MIDI2 event list API.
static void IOSHostMIDIReadProc(const MIDIPacketList *pktlist,
                                void *readProcRefCon,
                                void *srcConnRefCon)
{
    AUScheduleMIDIEventBlock block = (__bridge AUScheduleMIDIEventBlock)readProcRefCon;
    if (!block) return;
    const MIDIPacket *packet = &pktlist->packet[0];
    for (UInt32 i = 0; i < pktlist->numPackets; ++i)
    {
        if (packet->length > 0 && packet->length <= 3)
            block(AUEventSampleTimeImmediate, 0, packet->length, packet->data);
        packet = MIDIPacketNext(packet);
    }
}

@implementation IOSHostViewController

- (void)viewDidLoad
{
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    self.statusLabel = [[UILabel alloc] init];
    self.statusLabel.translatesAutoresizingMaskIntoConstraints = NO;
    self.statusLabel.text = @"Loading AUv3…";
    self.statusLabel.textAlignment = NSTextAlignmentCenter;
    self.statusLabel.numberOfLines = 0;
    [self.view addSubview:self.statusLabel];
    [NSLayoutConstraint activateConstraints:@[
        [self.statusLabel.centerXAnchor constraintEqualToAnchor:self.view.centerXAnchor],
        [self.statusLabel.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [self.statusLabel.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor constant:20],
        [self.statusLabel.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-20],
    ]];

    [self instantiateAU];
}

- (void)setStatus:(NSString *)msg
{
    self.statusLabel.text = msg;
    NSLog(@"[ios-host] %@", msg);
}

- (void)instantiateAU
{
    // Look up the factory class statically linked into our binary. The
    // name comes from target_add_auv3_standalone_ios_wrapper's MD5 of the
    // manufacturer+subtype codes — identical to what wrap_auv3.cmake uses
    // for the .appex, so the class symbol is the same.
    NSString *factoryName = @AUV3_FACTORY_CLASS_NAME_STR;
    if (factoryName.length == 0)
    {
        [self setStatus:@"No factory class name was compiled into the host. "
                        @"Check target_add_auv3_standalone_ios_wrapper CMake integration."];
        return;
    }
    Class factoryClass = NSClassFromString(factoryName);
    if (!factoryClass)
    {
        [self setStatus:[NSString stringWithFormat:
            @"Factory class %@ is not linked into this binary. "
             "The AUv3 wrapper sources must be compiled into the host target.",
            factoryName]];
        return;
    }
    if (![factoryClass conformsToProtocol:@protocol(AUAudioUnitFactory)])
    {
        [self setStatus:[NSString stringWithFormat:
            @"%@ does not conform to AUAudioUnitFactory.", factoryName]];
        return;
    }

    AudioComponentDescription desc = {};
    desc.componentType         = FourCCFromString(AU_TYPE_STR);
    desc.componentSubType      = FourCCFromString(AU_SUBTYPE_STR);
    desc.componentManufacturer = FourCCFromString(AU_MANUFACTURER_STR);

    // The factory class IS an AUViewController subclass that conforms to
    // AUAudioUnitFactory — alloc/init is safe without an NSExtensionContext;
    // beginRequestWithExtensionContext: is only invoked by the extension
    // runtime when the VC is hosted out-of-process.
    id<AUAudioUnitFactory> factory = [[factoryClass alloc] init];
    NSError *err = nil;
    AUAudioUnit *au = [factory createAudioUnitWithComponentDescription:desc error:&err];
    if (!au)
    {
        [self setStatus:[NSString stringWithFormat:@"AU creation failed: %@",
            err ? err.localizedDescription : @"unknown error"]];
        return;
    }
    self.audioUnit = au;
    self.pluginViewController = (UIViewController *)factory;

    // Audio graph setup. Routes the AU's render output to the system mixer
    // so notes actually produce sound. Effects (aufx) aren't currently
    // wired for input — this iOS host is an instrument host for the
    // common case (aumu / aufg). Kept simple on purpose.
    [self setupAudioGraph];

    // MIDI in. CoreMIDI connects external (USB/Lightning/BT) keyboards and
    // virtual sources from other apps on the same device. Without this the
    // synth hears nothing.
    [self setupMIDI];

    [self embedPluginVC:self.pluginViewController];
}

- (void)setupMIDI
{
    // An AU only accepts MIDI if it exposes scheduleMIDIEventBlock; pure
    // audio effects return nil here. aumu/aumi instruments set it.
    self.scheduleMIDIBlock = self.audioUnit.scheduleMIDIEventBlock;
    if (!self.scheduleMIDIBlock)
    {
        NSLog(@"[ios-host] AU does not accept MIDI (no scheduleMIDIEventBlock)");
        return;
    }

    MIDIClientRef client = 0;
    // MIDIClientCreateWithBlock (iOS 9+) is preferred over the C-callback
    // MIDIClientCreate because it lets us react to topology changes
    // (new USB keyboard plugged in after launch) inline.
    OSStatus st = MIDIClientCreateWithBlock(
        CFSTR("ClapWrapperIOSStandalone"), &client,
        ^(const MIDINotification *message) {
            if (message->messageID == kMIDIMsgSetupChanged)
                [self connectAllMIDISources];
        });
    if (st != noErr)
    {
        NSLog(@"[ios-host] MIDIClientCreateWithBlock failed: %d", (int)st);
        return;
    }
    self.midiClient = client;

    MIDIPortRef port = 0;
    st = MIDIInputPortCreate(client, CFSTR("Input"), IOSHostMIDIReadProc,
                             (__bridge void *)self.scheduleMIDIBlock, &port);
    if (st != noErr)
    {
        NSLog(@"[ios-host] MIDIInputPortCreate failed: %d", (int)st);
        return;
    }
    self.midiInputPort = port;

    [self connectAllMIDISources];
}

- (void)connectAllMIDISources
{
    if (!self.midiInputPort) return;
    ItemCount count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i)
    {
        MIDIEndpointRef src = MIDIGetSource(i);
        // Idempotent — CoreMIDI ignores a duplicate connect call with
        // kMIDIUnknownProperty status rather than creating two routes.
        MIDIPortConnectSource(self.midiInputPort, src, NULL);
    }
    NSLog(@"[ios-host] MIDI connected to %lu source(s)", (unsigned long)count);
}

- (void)dealloc
{
    if (_midiInputPort) MIDIPortDispose(_midiInputPort);
    if (_midiClient)    MIDIClientDispose(_midiClient);
}

- (void)setupAudioGraph
{
    NSError *err = nil;

    AVAudioSession *session = [AVAudioSession sharedInstance];
    [session setCategory:AVAudioSessionCategoryPlayback error:&err];
    if (err) NSLog(@"[ios-host] AVAudioSession setCategory: %@", err);
    err = nil;

    // Request a 256-frame I/O buffer for snappier note response. iOS
    // rounds to its nearest supported size (typically powers of two
    // between 64 and 4096); the actual value is read back after
    // setActive:. Must be set BEFORE activation for the hardware to
    // honor it on this session.
    const AVAudioFrameCount preferredFrames = 256;
    double rateForPrefs = session.sampleRate > 0 ? session.sampleRate : 48000.0;
    [session setPreferredIOBufferDuration:((double)preferredFrames / rateForPrefs)
                                    error:&err];
    if (err) NSLog(@"[ios-host] setPreferredIOBufferDuration: %@", err);
    err = nil;

    [session setActive:YES error:&err];
    if (err) NSLog(@"[ios-host] AVAudioSession setActive: %@", err);
    err = nil;

    NSLog(@"[ios-host] audio session: sampleRate=%.0f ioBuffer=%.3fms (~%.0f frames)",
          session.sampleRate,
          session.IOBufferDuration * 1000.0,
          session.IOBufferDuration * session.sampleRate);

    // Pin the AU's output bus to a stereo float32 format at the session's
    // current hardware rate. An AUv3 instrument's default is often mono
    // or a hardware-defined format that doesn't line up with what the
    // engine's mainMixer wants. Forcing stereo here keeps the graph
    // self-consistent.
    AUAudioUnitBus *outBus = self.audioUnit.outputBusses[0];
    double sr = session.sampleRate > 0 ? session.sampleRate : 48000.0;
    AVAudioFormat *stereoFmt = [[AVAudioFormat alloc]
        initStandardFormatWithSampleRate:sr channels:2];
    if (![outBus setFormat:stereoFmt error:&err])
        NSLog(@"[ios-host] outputBus setFormat: %@", err);
    err = nil;

    // AVAudioEngine can ask the source node for fairly large blocks; the
    // AU must accept at least that frame count or render fails. 4096 is
    // comfortably above any iOS session buffer size we're likely to see.
    self.audioUnit.maximumFramesToRender = 4096;

    if (![self.audioUnit allocateRenderResourcesAndReturnError:&err])
    {
        NSLog(@"[ios-host] allocateRenderResources failed: %@", err);
        return;
    }

    // Use renderBlock — NOT internalRenderBlock. renderBlock is the
    // public entry that wraps the internal block with parameter
    // automation + MIDI event scheduling; internalRenderBlock is what the
    // AU subclass *implements* and calling it directly skips the plumbing
    // that delivers MIDI events from scheduleMIDIEventBlock into the
    // render cycle.
    AURenderBlock auRender = self.audioUnit.renderBlock;

    AVAudioFormat *auFormat = outBus.format;
    AVAudioSourceNode *srcNode = [[AVAudioSourceNode alloc] initWithFormat:auFormat
        renderBlock:^OSStatus(BOOL *isSilence,
                              const AudioTimeStamp *timestamp,
                              AVAudioFrameCount frameCount,
                              AudioBufferList *outputData) {
            AudioUnitRenderActionFlags flags = 0;
            return auRender(&flags, timestamp, frameCount, 0, outputData, NULL);
        }];
    self.auSourceNode = srcNode;

    self.audioEngine = [[AVAudioEngine alloc] init];
    [self.audioEngine attachNode:srcNode];
    [self.audioEngine connect:srcNode
                           to:self.audioEngine.mainMixerNode
                       format:auFormat];

    if (![self.audioEngine startAndReturnError:&err])
        NSLog(@"[ios-host] AVAudioEngine start: %@", err);
}

- (void)embedPluginVC:(UIViewController *)vc
{
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
    self.statusLabel.hidden = YES;
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
