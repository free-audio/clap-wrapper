#import "AUv3HostAppDelegate.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>
#import <CoreMIDI/CoreMIDI.h>

#include <iostream>
#include <dlfcn.h>

// --- FourCC helper: convert a 4-char string like "aufx" to a uint32_t ---
static uint32_t fourCCFromString(const char *s)
{
  if (!s || strlen(s) < 4) return 0;
  return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 8) | (uint32_t)s[3];
}

// --- MIDI input state ---
static MIDIClientRef sMIDIClient = 0;
static MIDIPortRef sMIDIInputPort = 0;

@implementation AUv3HostAppDelegate
{
  AVAudioEngine *_engine;
  AVAudioUnit *_avAudioUnit;
  AUAudioUnit *_directAU;  // used when loading appex directly (no system registration)
  NSViewController *_auViewController;
  NSViewController *_factoryViewController;  // the factory/principal class VC from direct appex loading
  NSString *_settingsPath;

  // MIDI
  AUScheduleMIDIEventBlock _scheduleMIDIBlock;
}

// ---------------------------------------------------------------------------
#pragma mark - Application lifecycle
// ---------------------------------------------------------------------------

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
  [NSApp activateIgnoringOtherApps:YES];

  // Defer setup slightly so the window is visible
  [NSTimer scheduledTimerWithTimeInterval:0.001
                                   target:self
                                 selector:@selector(doSetup)
                                 userInfo:nil
                                  repeats:NO];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
  return YES;
}

- (void)applicationWillTerminate:(NSNotification *)notification
{
  [self teardownMIDI];
  [self saveState];

  // Remove KVO observer before tearing down
  if (_auViewController)
  {
    @try
    {
      [_auViewController removeObserver:self forKeyPath:@"preferredContentSize"];
    }
    @catch (NSException *e)
    {
      // Observer was never added (e.g., no GUI path)
    }
  }

  if (_engine)
  {
    [_engine stop];

    if (_avAudioUnit)
    {
      [_engine detachNode:_avAudioUnit];
    }
    _engine = nil;
  }

  if (_directAU)
  {
    [_directAU deallocateRenderResources];
    _directAU = nil;
  }

  _avAudioUnit = nil;
  _auViewController = nil;
}

// ---------------------------------------------------------------------------
#pragma mark - Appex Registration
// ---------------------------------------------------------------------------

- (NSBundle *)findEmbeddedAppexBundle
{
  NSString *plugInsPath = [[NSBundle mainBundle] builtInPlugInsPath];
  if (!plugInsPath) return nil;

  NSArray *contents = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:plugInsPath error:nil];
  for (NSString *item in contents)
  {
    if ([item hasSuffix:@".appex"])
    {
      NSString *appexPath = [plugInsPath stringByAppendingPathComponent:item];
      NSBundle *bundle = [NSBundle bundleWithPath:appexPath];
      if (bundle)
      {
        std::cout << "[auv3-standalone] Found appex bundle: " << [appexPath UTF8String] << std::endl;
        return bundle;
      }
    }
  }
  return nil;
}

- (AUAudioUnit *)instantiateAUDirectlyFromAppex:(NSBundle *)appexBundle
                           componentDescription:(AudioComponentDescription)desc
                                          error:(NSError **)outError
{
  // Load the appex bundle to get its Objective-C classes.
  // Note: MH_EXECUTE binaries can't be loaded via NSBundle's load method,
  // so we use dlopen on the executable directly to register the ObjC classes.
  if (![appexBundle isLoaded])
  {
    NSString *execPath = [appexBundle executablePath];
    if (execPath)
    {
      void *handle = dlopen([execPath fileSystemRepresentation], RTLD_NOW | RTLD_LOCAL);
      if (!handle)
      {
        std::cout << "[auv3-standalone] dlopen fallback failed: " << dlerror() << std::endl;
      }
      else
      {
        std::cout << "[auv3-standalone] Appex loaded via dlopen" << std::endl;
      }
    }

    // Also try NSBundle load (works for MH_BUNDLE/MH_DYLIB)
    if (![appexBundle isLoaded])
    {
      NSError *loadError = nil;
      if (![appexBundle loadAndReturnError:&loadError])
      {
        std::cout << "[auv3-standalone] WARNING: NSBundle load failed: " <<
            [loadError.localizedDescription UTF8String] << " (may be expected for executable appex)"
                  << std::endl;
      }
      else
      {
        std::cout << "[auv3-standalone] Appex bundle loaded via NSBundle" << std::endl;
      }
    }
  }

  // Get the principal class from the appex's Info.plist
  NSDictionary *plist = appexBundle.infoDictionary;
  NSDictionary *nsExt = plist[@"NSExtension"];
  NSString *principalClassName = nsExt[@"NSExtensionPrincipalClass"];

  if (!principalClassName)
  {
    std::cout << "[auv3-standalone] ERROR: No NSExtensionPrincipalClass in appex" << std::endl;
    if (outError)
      *outError =
          [NSError errorWithDomain:@"ClapAUv3"
                              code:-1
                          userInfo:@{NSLocalizedDescriptionKey : @"No principal class in appex"}];
    return nil;
  }

  std::cout << "[auv3-standalone] Principal class: " << [principalClassName UTF8String] << std::endl;

  // Get the factory class
  Class factoryClass = NSClassFromString(principalClassName);
  if (!factoryClass)
  {
    // Try loading from the bundle explicitly
    factoryClass = [appexBundle classNamed:principalClassName];
  }

  if (!factoryClass)
  {
    std::cout << "[auv3-standalone] ERROR: Cannot find class " << [principalClassName UTF8String]
              << std::endl;
    if (outError)
      *outError = [NSError errorWithDomain:@"ClapAUv3"
                                      code:-2
                                  userInfo:@{
                                    NSLocalizedDescriptionKey : [NSString
                                        stringWithFormat:@"Cannot find class %@", principalClassName]
                                  }];
    return nil;
  }

  // The factory class conforms to AUAudioUnitFactory
  if (![factoryClass conformsToProtocol:@protocol(AUAudioUnitFactory)])
  {
    std::cout << "[auv3-standalone] ERROR: Principal class does not conform to AUAudioUnitFactory"
              << std::endl;
    if (outError)
      *outError =
          [NSError errorWithDomain:@"ClapAUv3"
                              code:-3
                          userInfo:@{
                            NSLocalizedDescriptionKey : @"Principal class is not an AUAudioUnitFactory"
                          }];
    return nil;
  }

  // Create the factory and ask it to create the AU.
  // The factory IS also the view controller (AUViewController subclass), so keep it alive.
  id<AUAudioUnitFactory> factory = [[factoryClass alloc] init];
  _factoryViewController = (NSViewController *)factory;
  AUAudioUnit *au = [factory createAudioUnitWithComponentDescription:desc error:outError];

  if (!au)
  {
    std::cout << "[auv3-standalone] ERROR: Factory returned nil" << std::endl;
    return nil;
  }

  std::cout << "[auv3-standalone] AUAudioUnit created directly from appex" << std::endl;
  return au;
}

// ---------------------------------------------------------------------------
#pragma mark - Setup
// ---------------------------------------------------------------------------

- (void)doSetup
{
#if __MAC_OS_X_VERSION_MIN_REQUIRED >= 101400
  // Request microphone permission. Setup continues immediately (the TCC
  // dialog is async), so on first launch the engine is built WITHOUT the
  // input node — rebuild it once the user grants access, otherwise an
  // effect processes silence until the app is relaunched.
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio])
  {
    case AVAuthorizationStatusNotDetermined:
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                               completionHandler:^(BOOL granted) {
                                 if (!granted) return;
                                 dispatch_async(dispatch_get_main_queue(), ^{
                                   if (self->_engine && self->_avAudioUnit)
                                   {
                                     std::cout << "[auv3-standalone] microphone access granted — "
                                                  "rebuilding engine with input"
                                               << std::endl;
                                     [self->_engine stop];
                                     [self setupEngine];
                                   }
                                 });
                               }];
      break;
    default:
      break;
  }
#endif

  // Build the AudioComponentDescription from compile-time defines
  AudioComponentDescription desc;
  desc.componentType = fourCCFromString(AU_TYPE_STR);
  desc.componentSubType = fourCCFromString(AU_SUBTYPE_STR);
  desc.componentManufacturer = fourCCFromString(AU_MANUFACTURER_STR);
  desc.componentFlags = 0;
  desc.componentFlagsMask = 0;

  std::cout << "[auv3-standalone] Looking for AU: type='" << AU_TYPE_STR << "' subtype='"
            << AU_SUBTYPE_STR << "' manufacturer='" << AU_MANUFACTURER_STR << "'" << std::endl;

  // First try: use the system-registered AU (via AVAudioUnit + AVAudioEngine)
  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  if (comp)
  {
    CFStringRef compName = NULL;
    AudioComponentCopyName(comp, &compName);
    std::cout << "[auv3-standalone] Found registered AudioComponent: "
              << (compName ? [(__bridge NSString *)compName UTF8String] : "?") << std::endl;
    if (compName) CFRelease(compName);

    __weak typeof(self) weakSelf = self;
    [AVAudioUnit instantiateWithComponentDescription:desc
                                             options:kAudioComponentInstantiation_LoadInProcess
                                   completionHandler:^(AVAudioUnit *_Nullable audioUnit,
                                                       NSError *_Nullable error) {
                                     dispatch_async(dispatch_get_main_queue(), ^{
                                       __strong typeof(weakSelf) self = weakSelf;
                                       if (self) [self finishSetupWithAudioUnit:audioUnit error:error];
                                     });
                                   }];
    return;
  }

  // Fallback: load the appex bundle directly (in-process, no AVAudioEngine)
  std::cout << "[auv3-standalone] AudioComponent not registered, loading appex directly" << std::endl;

  NSBundle *appexBundle = [self findEmbeddedAppexBundle];
  if (!appexBundle)
  {
    std::cout << "[auv3-standalone] ERROR: No embedded appex found" << std::endl;
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Failed to load Audio Unit"];
    [alert setInformativeText:@"No embedded .appex found in PlugIns directory"];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    return;
  }

  NSError *error = nil;
  AUAudioUnit *au = [self instantiateAUDirectlyFromAppex:appexBundle
                                    componentDescription:desc
                                                   error:&error];
  if (!au)
  {
    NSString *msg = error ? error.localizedDescription : @"Unknown error creating AU from appex";
    std::cout << "[auv3-standalone] ERROR: " << [msg UTF8String] << std::endl;
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Failed to load Audio Unit"];
    [alert setInformativeText:msg];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    return;
  }

  // Wrap the raw AUAudioUnit in an AVAudioUnit for use with AVAudioEngine
  // We need to use AVAudioUnit's instantiation since AVAudioEngine requires AVAudioUnit nodes.
  // Since the AU isn't registered, we can't use AVAudioUnit directly.
  // Instead, we'll work with the AUAudioUnit directly (without AVAudioEngine).
  [self finishSetupWithAUAudioUnit:au];
}

// ---------------------------------------------------------------------------
#pragma mark - Finish Setup
// ---------------------------------------------------------------------------

- (void)finishSetupWithAudioUnit:(AVAudioUnit *)audioUnit error:(NSError *)error
{
  if (error || !audioUnit)
  {
    NSString *msg = error ? error.localizedDescription : @"Unknown error";
    std::cout << "[auv3-standalone] ERROR: Failed to instantiate AU: " << [msg UTF8String] << std::endl;
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Failed to load Audio Unit"];
    [alert setInformativeText:msg];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    return;
  }

  _avAudioUnit = audioUnit;
  std::cout << "[auv3-standalone] AU instantiated via AVAudioUnit: " << [audioUnit.name UTF8String]
            << std::endl;

  [self restoreState];
  [self setupEngine];
  [self setupGUI];
  [self setupMIDI];
}

- (void)finishSetupWithAUAudioUnit:(AUAudioUnit *)au
{
  // Direct appex loading path: we have an AUAudioUnit but no AVAudioUnit.
  // We can still set up audio I/O using the AUAudioUnit's render block directly,
  // and request the view controller for GUI.

  std::cout << "[auv3-standalone] Setting up with direct AUAudioUnit (no AVAudioEngine)" << std::endl;

  // Store the raw AU -- we need to keep it alive
  _directAU = au;

  // Allocate render resources
  NSError *error = nil;
  if (![au allocateRenderResourcesAndReturnError:&error])
  {
    std::cout << "[auv3-standalone] ERROR: allocateRenderResources failed: " <<
        [error.localizedDescription UTF8String] << std::endl;
  }
  else
  {
    std::cout << "[auv3-standalone] Render resources allocated" << std::endl;
  }

  // Set up the GUI from the AUAudioUnit directly
  [self setupGUIFromAUAudioUnit:au];
  [self setupMIDIForAUAudioUnit:au];
}

// ---------------------------------------------------------------------------
#pragma mark - AVAudioEngine
// ---------------------------------------------------------------------------

- (void)setupEngine
{
  uint32_t auType = fourCCFromString(AU_TYPE_STR);

  // A MIDI processor (aumi) may expose no audio output busses at all —
  // connecting it into the audio graph would throw. Its MIDI path doesn't
  // need the engine graph, so skip it entirely.
  if (_avAudioUnit.AUAudioUnit.outputBusses.count == 0)
  {
    std::cout << "[auv3-standalone] AU has no audio output busses — skipping audio engine graph"
              << std::endl;
    return;
  }

  BOOL wantInput = (auType == kAudioUnitType_Effect || auType == kAudioUnitType_MIDIProcessor) &&
                   _avAudioUnit.AUAudioUnit.inputBusses.count > 0;

  // Decide whether the hardware input is usable BEFORE building the real
  // engine. Merely accessing engine.inputNode binds the hardware input to
  // the engine's IO unit — if input is unavailable (microphone permission
  // not granted, no input device), connecting it throws NSException
  // ('Input HW format is invalid' -> abort), and even an unconnected but
  // touched input node makes engine start fail with kAudioHardwareBad-
  // DeviceError. So: check permission first, then probe the HW format on
  // a throwaway engine, and never touch the real engine's inputNode
  // unless input is actually usable. Without input the AU's input pull
  // fails and the wrapper substitutes silence, so the app still runs.
  BOOL inputUsable = NO;
  if (wantInput)
  {
    if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] ==
        AVAuthorizationStatusAuthorized)
    {
      AVAudioEngine *probe = [[AVAudioEngine alloc] init];
      AVAudioFormat *hwFormat = [probe.inputNode inputFormatForBus:0];
      inputUsable = (hwFormat.sampleRate > 0 && hwFormat.channelCount > 0);
      if (!inputUsable)
      {
        std::cout << "[auv3-standalone] WARNING: no usable input device (HW format "
                  << hwFormat.sampleRate << " Hz / " << hwFormat.channelCount
                  << " ch) — running with silent input" << std::endl;
      }
    }
    else
    {
      std::cout << "[auv3-standalone] WARNING: microphone access not granted — "
                   "running with silent input"
                << std::endl;
    }
  }

  _engine = [[AVAudioEngine alloc] init];
  [_engine attachNode:_avAudioUnit];

  AVAudioNode *output = _engine.outputNode;
  AVAudioFormat *outputFormat = [output inputFormatForBus:0];

  // AVAudioEngine throws NSException (-> abort) on invalid graph
  // configurations instead of returning NSErrors. Convert that into a
  // visible alert instead of a crash report.
  @try
  {
    if (inputUsable)
    {
      // Effect: input -> AU -> output
      AVAudioNode *input = _engine.inputNode;
      AVAudioFormat *inputFormat = [input outputFormatForBus:0];
      [_engine connect:input to:_avAudioUnit format:inputFormat];
    }
    [_engine connect:_avAudioUnit to:output format:outputFormat];
  }
  @catch (NSException *e)
  {
    std::cout << "[auv3-standalone] ERROR: engine graph setup failed: " << [e.reason UTF8String]
              << std::endl;
    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Failed to set up audio engine"];
    [alert setInformativeText:e.reason ?: @"Invalid audio graph configuration"];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
    return;
  }

  NSError *error = nil;
  if (![_engine startAndReturnError:&error])
  {
    std::cout << "[auv3-standalone] ERROR: Failed to start engine: " <<
        [error.localizedDescription UTF8String] << std::endl;

    NSAlert *alert = [[NSAlert alloc] init];
    [alert setMessageText:@"Failed to start audio engine"];
    [alert setInformativeText:error.localizedDescription];
    [alert addButtonWithTitle:@"OK"];
    [alert runModal];
  }
  else
  {
    std::cout << "[auv3-standalone] Engine started. Sample rate: " << outputFormat.sampleRate << " Hz"
              << std::endl;
  }
}

// ---------------------------------------------------------------------------
#pragma mark - GUI
// ---------------------------------------------------------------------------

- (void)setupGUI
{
  AUAudioUnit *au = _avAudioUnit.AUAudioUnit;

  [au requestViewControllerWithCompletionHandler:^(AUViewControllerBase *vc) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (!vc)
      {
        std::cout << "[auv3-standalone] No view controller provided by AU" << std::endl;
        // Show the window as-is (empty) -- the plugin has no GUI
        [[self window] orderFrontRegardless];
        return;
      }

      self->_auViewController = vc;

      // Observe preferredContentSize — the AU view controller sets this
      // asynchronously when the CLAP GUI is created (after viewDidMoveToWindow).
      [vc addObserver:self
           forKeyPath:@"preferredContentSize"
              options:NSKeyValueObservingOptionNew
              context:NULL];

      NSSize preferredSize = vc.preferredContentSize;
      if (preferredSize.width < 1 || preferredSize.height < 1)
      {
        preferredSize = NSMakeSize(480, 360);
      }

      NSWindow *window = [self window];
      [window setContentSize:preferredSize];
      [window setDelegate:self];
      window.styleMask |= NSWindowStyleMaskResizable;
      window.contentMinSize = NSMakeSize(100, 100);

      NSView *contentView = [window contentView];
      NSView *auView = vc.view;
      auView.frame = contentView.bounds;
      auView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      [contentView addSubview:auView];

      [window setMovableByWindowBackground:YES];
      [window orderFrontRegardless];

      std::cout << "[auv3-standalone] window styleMask=0x" << std::hex << (unsigned long)window.styleMask
                << std::dec
                << " resizable=" << ((window.styleMask & NSWindowStyleMaskResizable) ? "YES" : "NO")
                << std::endl;

      std::cout << "[auv3-standalone] GUI displayed (" << (int)preferredSize.width << "x"
                << (int)preferredSize.height << ")" << std::endl;

      // For out-of-process AUv3, KVO on preferredContentSize may not work
      // across the XPC boundary. Poll after a delay to pick up the plugin's
      // actual GUI size once it has been created (via viewDidAppear).
      [self _pollPreferredContentSize:vc retries:10];
    });
  }];
}

- (void)setupGUIFromAUAudioUnit:(AUAudioUnit *)au
{
  // Use the factory VC directly — it IS the view controller (AUViewController subclass)
  // and already has audioUnit set from createAudioUnitWithComponentDescription:.
  NSViewController *vc = _factoryViewController;
  if (!vc)
  {
    std::cout << "[auv3-standalone] No factory view controller available (direct)" << std::endl;
    [[self window] orderFrontRegardless];
    return;
  }

  dispatch_async(dispatch_get_main_queue(), ^{
    self->_auViewController = vc;

    // Observe preferredContentSize — the AU view controller sets this
    // asynchronously when the CLAP GUI is created (after viewDidMoveToWindow).
    [vc addObserver:self
         forKeyPath:@"preferredContentSize"
            options:NSKeyValueObservingOptionNew
            context:NULL];

    NSSize preferredSize = vc.preferredContentSize;
    if (preferredSize.width < 1 || preferredSize.height < 1)
    {
      preferredSize = NSMakeSize(480, 360);
    }

    NSWindow *window = [self window];
    [window setContentSize:preferredSize];
    [window setDelegate:self];
    // Ensure the window is resizable
    window.styleMask |= NSWindowStyleMaskResizable;

    NSView *contentView = [window contentView];
    NSView *auView = vc.view;
    auView.frame = contentView.bounds;
    // Do NOT set autoresizingMask — it fights with explicit frame changes
    // from the plugin (which sets self.view.frame to the GUI size).
    // Window sizing is managed explicitly via _resizeWindowToFitGUI.
    auView.autoresizingMask = 0;
    [contentView addSubview:auView];

    [window orderFrontRegardless];

    std::cout << "[auv3-standalone] GUI displayed (direct) (" << (int)preferredSize.width << "x"
              << (int)preferredSize.height << ")" << std::endl;

    [self _pollPreferredContentSize:vc retries:10];
  });
}

- (void)setupMIDIForAUAudioUnit:(AUAudioUnit *)au
{
  _scheduleMIDIBlock = au.scheduleMIDIEventBlock;
  if (!_scheduleMIDIBlock)
  {
    std::cout << "[auv3-standalone] AU does not accept MIDI (direct)" << std::endl;
    return;
  }

  // Reuse the same MIDI setup logic
  OSStatus status = MIDIClientCreate(CFSTR("ClapWrapperAUv3Standalone"), NULL, NULL, &sMIDIClient);
  if (status != noErr) return;

  status = MIDIInputPortCreate(sMIDIClient, CFSTR("Input"), midiInputCallback,
                               (__bridge void *)_scheduleMIDIBlock, &sMIDIInputPort);
  if (status != noErr) return;

  ItemCount numSources = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < numSources; ++i)
  {
    MIDIEndpointRef src = MIDIGetSource(i);
    MIDIPortConnectSource(sMIDIInputPort, src, NULL);
  }
  std::cout << "[auv3-standalone] MIDI connected (direct) to " << numSources << " source(s)"
            << std::endl;
}

// ---------------------------------------------------------------------------
#pragma mark - MIDI
// ---------------------------------------------------------------------------

static void midiInputCallback(const MIDIPacketList *pktlist, void *readProcRefCon, void *srcConnRefCon)
{
  AUScheduleMIDIEventBlock block = (__bridge AUScheduleMIDIEventBlock)readProcRefCon;
  if (!block) return;

  const MIDIPacket *packet = &pktlist->packet[0];
  for (UInt32 i = 0; i < pktlist->numPackets; ++i)
  {
    if (packet->length > 0 && packet->length <= 3)
    {
      block(AUEventSampleTimeImmediate, 0, packet->length, packet->data);
    }
    packet = MIDIPacketNext(packet);
  }
}

- (void)setupMIDI
{
  _scheduleMIDIBlock = _avAudioUnit.AUAudioUnit.scheduleMIDIEventBlock;
  if (!_scheduleMIDIBlock)
  {
    std::cout << "[auv3-standalone] AU does not accept MIDI" << std::endl;
    return;
  }

  OSStatus status = MIDIClientCreate(CFSTR("ClapWrapperAUv3Standalone"), NULL, NULL, &sMIDIClient);
  if (status != noErr)
  {
    std::cout << "[auv3-standalone] Failed to create MIDI client: " << status << std::endl;
    return;
  }

  status = MIDIInputPortCreate(sMIDIClient, CFSTR("Input"), midiInputCallback,
                               (__bridge void *)_scheduleMIDIBlock, &sMIDIInputPort);
  if (status != noErr)
  {
    std::cout << "[auv3-standalone] Failed to create MIDI input port: " << status << std::endl;
    return;
  }

  // Connect to all available MIDI sources
  ItemCount numSources = MIDIGetNumberOfSources();
  for (ItemCount i = 0; i < numSources; ++i)
  {
    MIDIEndpointRef src = MIDIGetSource(i);
    MIDIPortConnectSource(sMIDIInputPort, src, NULL);
  }

  std::cout << "[auv3-standalone] MIDI connected to " << numSources << " source(s)" << std::endl;
}

- (void)teardownMIDI
{
  if (sMIDIInputPort)
  {
    MIDIPortDispose(sMIDIInputPort);
    sMIDIInputPort = 0;
  }
  if (sMIDIClient)
  {
    MIDIClientDispose(sMIDIClient);
    sMIDIClient = 0;
  }
  _scheduleMIDIBlock = nil;
}

// ---------------------------------------------------------------------------
#pragma mark - State persistence
// ---------------------------------------------------------------------------

- (NSString *)settingsDirectory
{
  if (!_settingsPath)
  {
    NSArray *paths =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *appSupport = [paths firstObject];
    _settingsPath = [appSupport stringByAppendingPathComponent:@"clap-wrapper-auv3-standalone"];

    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm fileExistsAtPath:_settingsPath])
    {
      [fm createDirectoryAtPath:_settingsPath withIntermediateDirectories:YES attributes:nil error:nil];
    }
  }
  return _settingsPath;
}

- (NSString *)settingsFilePath
{
  NSString *auName = _avAudioUnit.name ?: @"unknown";
  // Sanitize name for filesystem
  NSCharacterSet *illegal = [NSCharacterSet characterSetWithCharactersInString:@"/\\:"];
  auName = [[auName componentsSeparatedByCharactersInSet:illegal] componentsJoinedByString:@"_"];
  return [[self settingsDirectory]
      stringByAppendingPathComponent:[NSString stringWithFormat:@"%@.plist", auName]];
}

- (void)saveState
{
  if (!_avAudioUnit) return;

  NSDictionary *state = _avAudioUnit.AUAudioUnit.fullState;
  if (state)
  {
    NSString *path = [self settingsFilePath];
    [state writeToFile:path atomically:YES];
    std::cout << "[auv3-standalone] State saved to " << [path UTF8String] << std::endl;
  }
}

- (void)restoreState
{
  if (!_avAudioUnit) return;

  NSString *path = [self settingsFilePath];
  NSDictionary *state = [NSDictionary dictionaryWithContentsOfFile:path];
  if (state)
  {
    _avAudioUnit.AUAudioUnit.fullState = state;
    std::cout << "[auv3-standalone] State restored from " << [path UTF8String] << std::endl;
  }
}

// ---------------------------------------------------------------------------
#pragma mark - NSWindowDelegate
// ---------------------------------------------------------------------------

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)frameSize
{
  // Let the window resize freely; the AU view uses autoresizing
  return frameSize;
}

// ---------------------------------------------------------------------------
#pragma mark - GUI size tracking
// ---------------------------------------------------------------------------

- (void)_resizeWindowToFitGUI:(NSViewController *)vc
{
  NSSize size = vc.preferredContentSize;
  NSWindow *window = [self window];
  if (size.width <= 0 || size.height <= 0 || !window) return;

  // Resize keeping top-left corner fixed
  NSRect oldFrame = window.frame;
  NSRect contentRect = NSMakeRect(0, 0, size.width, size.height);
  NSRect newFrame = [window frameRectForContentRect:contentRect];
  newFrame.origin.x = oldFrame.origin.x;
  newFrame.origin.y = oldFrame.origin.y + oldFrame.size.height - newFrame.size.height;

  [window setFrame:newFrame display:YES animate:NO];

  vc.view.frame = NSMakeRect(0, 0, size.width, size.height);
  [vc.view setNeedsDisplay:YES];
  [[window contentView] setNeedsDisplay:YES];
}

- (void)_pollPreferredContentSize:(NSViewController *)vc retries:(int)retries
{
  if (retries <= 0)
  {
    // Last resort: re-request the VC from the AU to get a fresh proxy
    // with updated preferredContentSize.
    if (_avAudioUnit)
    {
      [_avAudioUnit.AUAudioUnit
          requestViewControllerWithCompletionHandler:^(AUViewControllerBase *freshVC) {
            dispatch_async(dispatch_get_main_queue(), ^{
              if (freshVC)
              {
                NSSize size = freshVC.preferredContentSize;
                std::cout << "[auv3-standalone] Re-requested VC preferredContentSize: "
                          << (int)size.width << "x" << (int)size.height << std::endl;
                if (size.width > 0 && size.height > 0)
                {
                  self->_auViewController.preferredContentSize = size;
                  [self _resizeWindowToFitGUI:self->_auViewController];
                }
              }
            });
          }];
    }
    return;
  }

  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(500 * NSEC_PER_MSEC)),
                 dispatch_get_main_queue(), ^{
                   NSSize size = vc.preferredContentSize;
                   NSSize windowContent = [[self window] contentView].frame.size;

                   if (size.width > 0 && size.height > 0 &&
                       ((int)size.width != (int)windowContent.width ||
                        (int)size.height != (int)windowContent.height))
                   {
                     [self _resizeWindowToFitGUI:vc];
                   }
                   else
                   {
                     [self _pollPreferredContentSize:vc retries:retries - 1];
                   }
                 });
}

// ---------------------------------------------------------------------------
#pragma mark - KVO
// ---------------------------------------------------------------------------

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context
{
  if ([keyPath isEqualToString:@"preferredContentSize"] && object == _auViewController)
  {
    NSSize size = [(NSViewController *)object preferredContentSize];
    std::cout << "[auv3-standalone] preferredContentSize changed to " << (int)size.width << "x"
              << (int)size.height << std::endl;
    if (size.width > 0 && size.height > 0)
    {
      dispatch_async(dispatch_get_main_queue(), ^{
        [self _resizeWindowToFitGUI:self->_auViewController];
      });
    }
  }
}

@end
