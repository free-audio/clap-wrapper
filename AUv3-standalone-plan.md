# AUv3 Standalone Host App Plan

## Context

The AUv3 wrapper (`.appex`) is now implemented and builds successfully. However, AUv3 plugins on macOS benefit from being embedded in a containing host app for:

1. **Distribution** -- App Store requires a containing app for app extensions
2. **Registration** -- The system discovers the `.appex` through the containing app
3. **Self-testing** -- A standalone host lets developers test their AUv3 without a DAW
4. **User experience** -- Users get a standalone app with audio I/O, like the existing standalone wrapper

The goal is a macOS standalone app (`.app`) that:
- Embeds the `.appex` in its `PlugIns/` directory
- Instantiates the AUv3 via `AUAudioUnit` (not CLAP directly -- tests the real AUv3 path)
- Provides audio I/O via Core Audio (AVAudioEngine)
- Displays the plugin's GUI via `requestViewControllerWithCompletionHandler:`
- Handles MIDI input

This is fundamentally different from the existing standalone wrapper which loads the CLAP plugin directly using `Clap::Plugin` and RtAudio/RtMidi. The AUv3 standalone hosts the plugin **through the AUv3 API**, testing the full wrapper round-trip.

---

## Architecture

### Why AVAudioEngine (not RtAudio)

The existing standalone uses RtAudio/RtMidi for audio/MIDI I/O. For the AUv3 host, we use `AVAudioEngine` instead because:

- `AVAudioEngine` natively instantiates and connects `AUAudioUnit` nodes -- no manual buffer management needed
- It handles the render graph, sample rate negotiation, and buffer size automatically
- It integrates with Core MIDI for MIDI input
- It's the canonical way to host AUv3 plugins on macOS/iOS
- It keeps the host app minimal (~200 lines of Objective-C++ vs ~1500 lines for RtAudio+CLAP glue)

### Host App Flow

```
App launches
  -> Register embedded .appex (automatic via bundle discovery)
  -> Instantiate AUAudioUnit from AudioComponentDescription
  -> Connect to AVAudioEngine (input -> AU -> output)
  -> Request view controller for GUI
  -> Start engine
  -> Run until quit
```

### Bundle Structure

```
PluginName AUv3.app/
  Contents/
    Info.plist
    MacOS/
      PluginName AUv3
    Resources/
      MainMenu.nib
    PlugIns/
      PluginName.appex/        <-- The AUv3 plugin
        Contents/
          Info.plist
          MacOS/
            PluginName
```

---

## Implementation

### New Files

```
cmake/wrap_auv3_standalone.cmake                    -- CMake function
src/detail/standalone/macos/auv3/                    -- AUv3 host app sources
src/detail/standalone/macos/auv3/AUv3HostAppDelegate.mm  -- App delegate
src/detail/standalone/macos/auv3/AUv3HostAppDelegate.h   -- App delegate header
src/detail/standalone/macos/auv3/Info.plist.in       -- App bundle plist
src/detail/standalone/macos/auv3/MainMenu.xib        -- Menu bar XIB
src/wrapasauv3standalone.mm                          -- main() entry point
```

### Files to Modify

```
cmake/wrapper_functions.cmake     -- include(cmake/wrap_auv3_standalone.cmake)
cmake/make_clapfirst.cmake        -- Add AUV3_STANDALONE to format/standalone options
```

---

### Step 1: CMake function `target_add_auv3_standalone_wrapper()`

File: `cmake/wrap_auv3_standalone.cmake`

```cmake
function(target_add_auv3_standalone_wrapper)
    Arguments:
      TARGET             -- The executable target
      OUTPUT_NAME        -- App display name
      BUNDLE_IDENTIFIER  -- macOS bundle ID
      BUNDLE_VERSION     -- Version string
      AUV3_TARGET        -- The .appex target to embed
      MACOS_ICON         -- Optional icon

      # Audio Unit identification (to instantiate the correct AU)
      AU_TYPE            -- e.g. "aufx", "aumu", "aumi"
      AU_SUBTYPE         -- 4-char subtype code
      AU_MANUFACTURER    -- 4-char manufacturer code
```

The function:
1. Creates the executable target as a macOS app bundle
2. Adds a post-build step to copy `${AUV3_TARGET}.appex` into `Contents/PlugIns/`
3. Compiles the XIB to NIB
4. Links against `AVFoundation`, `AudioToolbox`, `CoreAudio`, `CoreMIDI`, `AppKit`
5. Does **not** link against RtAudio/RtMidi or `clap-wrapper-shared-detail` (the host doesn't touch CLAP directly)
6. Passes `AU_TYPE`, `AU_SUBTYPE`, `AU_MANUFACTURER` as compile definitions so the app knows which AudioComponent to instantiate

### Step 2: Entry point

File: `src/wrapasauv3standalone.mm`

```objc
int main(int argc, const char *argv[]) {
    return NSApplicationMain(argc, argv);
}
```

### Step 3: MainMenu XIB

File: `src/detail/standalone/macos/auv3/MainMenu.xib`

Copy from the existing standalone XIB (`src/detail/standalone/macos/MainMenu.xib`), simplified:
- App menu: About, Quit
- Window menu: Minimize, Zoom
- No File menu (no .cwstream save/load -- the AUv3 state is managed by the AU host API)
- No Audio/MIDI Settings (AVAudioEngine uses system defaults, or we add a simple settings panel later)

### Step 4: App Delegate -- the core

File: `src/detail/standalone/macos/auv3/AUv3HostAppDelegate.mm`

This is the heart of the standalone. It uses `AVAudioEngine` + `AVAudioUnit` to host the AUv3.

```objc
@interface AUv3HostAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property (nonatomic) AVAudioEngine *engine;
@property (nonatomic) AVAudioUnit *avAudioUnit;
@property (nonatomic, weak) IBOutlet NSWindow *window;
@end
```

#### `applicationDidFinishLaunching:` flow:

1. **Find the AudioComponent** matching the compiled-in type/subtype/manufacturer:
   ```objc
   AudioComponentDescription desc = {
       .componentType = AU_TYPE_FOURCC,
       .componentSubType = AU_SUBTYPE_FOURCC,
       .componentManufacturer = AU_MANUFACTURER_FOURCC,
       .componentFlags = 0,
       .componentFlagsMask = 0
   };
   ```

2. **Instantiate via AVAudioUnit**:
   ```objc
   [AVAudioUnit instantiateWithComponentDescription:desc
                                            options:kAudioComponentInstantiation_LoadInProcess
                               completionHandler:^(AVAudioUnit *unit, NSError *error) {
       self.avAudioUnit = unit;
       [self setupEngine];
       [self setupGUI];
   }];
   ```

3. **Set up AVAudioEngine** (in `setupEngine`):
   ```objc
   self.engine = [[AVAudioEngine alloc] init];
   [self.engine attachNode:self.avAudioUnit];

   // For effects (aufx): input -> AU -> output
   AVAudioNode *input = self.engine.inputNode;
   AVAudioNode *output = self.engine.outputNode;
   AVAudioFormat *format = [output outputFormatForBus:0];

   [self.engine connect:input to:self.avAudioUnit format:format];
   [self.engine connect:self.avAudioUnit to:output format:format];

   // For instruments (aumu): AU -> output (no input connection)
   // Determined at runtime from AU_TYPE

   NSError *error = nil;
   [self.engine startAndReturnError:&error];
   ```

4. **Request the GUI** (in `setupGUI`):
   ```objc
   [self.avAudioUnit.AUAudioUnit
       requestViewControllerWithCompletionHandler:^(AUViewControllerBase *vc) {
       if (vc) {
           NSSize size = vc.preferredContentSize;
           [[self window] setContentSize:size];
           [[self window] contentView].subviews = @[];
           [vc.view setFrame:[[self window] contentView].bounds];
           [[self window] contentView] addSubview:vc.view];
           // Observe preferredContentSize for resize
       }
   }];
   ```

5. **Handle MIDI** -- AVAudioEngine with `AVAudioUnitMIDIInstrument` handles MIDI routing automatically for instrument types. For generic MIDI input to effects, use CoreMIDI:
   ```objc
   // Simple approach: connect all MIDI sources
   // The AUAudioUnit receives MIDI through its scheduleMIDIEventBlock
   ```

#### `applicationWillTerminate:` flow:

1. Stop the AVAudioEngine
2. Detach the audio unit node
3. Release resources

#### State save/restore:

The `AUAudioUnit.fullState` property is used:
- On quit: save `fullState` to `~/Library/Application Support/clap-wrapper-auv3-standalone/{id}/settings.plist`
- On launch: restore from the saved plist after instantiation

### Step 5: Info.plist

File: `src/detail/standalone/macos/auv3/Info.plist.in`

Standard macOS app plist (same structure as existing standalone), with:
```xml
<key>NSMicrophoneUsageDescription</key>
<string>This app needs microphone access for audio input.</string>
```

No `NSExtension` key (this is the host app, not the extension).

### Step 6: Wire into make_clapfirst

In `cmake/make_clapfirst.cmake`, add an `AUV3_STANDALONE_CONFIGURATIONS` option (similar to `STANDALONE_CONFIGURATIONS`), or simply auto-generate an AUv3 standalone whenever an AUv3 target is built:

```cmake
if (APPLE AND ${BUILD_AUV3} GREATER -1 AND DEFINED C1ST_STANDALONE_CONFIGURATIONS)
    # For each standalone config, also create an AUv3 standalone
    set(AUV3SA_TARGET "${C1ST_TARGET_NAME}_auv3_standalone")
    add_executable(${AUV3SA_TARGET})
    target_add_auv3_standalone_wrapper(
        TARGET ${AUV3SA_TARGET}
        OUTPUT_NAME "${C1ST_OUTPUT_NAME} AUv3"
        AUV3_TARGET ${AUV3_TARGET}
        AU_TYPE "${C1ST_AUV2_INSTRUMENT_TYPE}"
        AU_SUBTYPE "${C1ST_AUV2_SUBTYPE_CODE}"
        AU_MANUFACTURER "${C1ST_AUV2_MANUFACTURER_CODE}"
    )
    add_dependencies(${ALL_TARGET} ${AUV3SA_TARGET})
endif()
```

---

## What This Doesn't Do (Deferred)

- **iOS** -- The same approach works on iOS with `UIKit` instead of `AppKit`, but deferred
- **Audio Settings UI** -- AVAudioEngine uses system defaults; a settings panel can be added later
- **MIDI device selection** -- Initially connects to all available MIDI sources
- **Preset management** -- fullState save/restore covers basic persistence; factory presets deferred

## Verification

1. **Build**: `cmake --build . --target <plugin>_auv3_standalone` succeeds
2. **Launch**: The app opens, displays the plugin GUI
3. **Audio**: Audio passes through (effects) or generates (instruments)
4. **auval**: The embedded `.appex` is discoverable by `auval -a` when the app has been launched once
5. **DAW**: After launching the standalone once, the AUv3 appears in Logic Pro / GarageBand
6. **State**: Quit and relaunch preserves plugin state
