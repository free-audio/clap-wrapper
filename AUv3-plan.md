# AUv3 Wrapper Implementation Plan

## Context

The clap-wrapper project wraps CLAP audio plugins into other plugin formats. Currently supported: VST3, AUv2, AAX, Standalone. This plan adds **AUv3 (Audio Unit v3)** as a new wrapper format, targeting macOS first with iOS deferred but architecturally considered.

AUv3 is Apple's modern audio plugin API, replacing AUv2. Key differences from AUv2:
- Objective-C `AUAudioUnit` base class (not C++ `ausdk::AUBase`)
- `AUParameterTree` with KVO-based parameter observation (not C-style callbacks)
- Block-based render callback (`AURenderBlock`) with `AURenderEvent` linked list (not C function pointers)
- `AUViewController` for GUI (not `AUCocoaUIBase` protocol)
- App Extension packaging (`.appex`) with `AUAudioUnitFactory` protocol
- No dependency on Apple's AudioUnitSDK C++ library -- uses `AudioToolbox.framework` directly

The existing `Clap::Plugin` proxy, `Clap::IHost`, `Clap::IAutomation`, `Clap::StateMemento`, and `os::IPlugObject` are all reusable as the bridge layer between AUv3 and CLAP.

---

## Phase 1: Build System Skeleton

**Goal**: AUv3 target compiles and links as an empty `.appex` bundle.

### 1.1 Create `cmake/wrap_auv3.cmake`

New file. Define `target_add_auv3_wrapper()` following the pattern of `wrap_auv2.cmake`:

```
function(target_add_auv3_wrapper)
  Arguments:
    TARGET, OUTPUT_NAME, BUNDLE_IDENTIFIER, BUNDLE_VERSION,
    RESOURCE_DIRECTORY, MANUFACTURER_NAME, MANUFACTURER_CODE,
    SUBTYPE_CODE, INSTRUMENT_TYPE,
    CLAP_TARGET_FOR_CONFIG,
    MACOS_EMBEDDED_CLAP_LOCATION
```

Key differences from AUv2:
- **No `guarantee_auv2sdk()` call** -- AUv3 uses `AudioToolbox.framework` directly, not the AudioUnit C++ SDK
- Frameworks: `AudioToolbox`, `AVFoundation`, `CoreAudio`, `CoreMIDI`, `Foundation`, `AppKit` (macOS) / `UIKit` (iOS, future)
- Bundle extension: `.appex` (not `.component`)
- Info.plist requires `NSExtension` dictionary with `NSExtensionPointIdentifier = com.apple.AudioUnit-UI`

Files to modify:
- `cmake/wrapper_functions.cmake` -- add `include(cmake/wrap_auv3.cmake)` after the auv2 line

### 1.2 Create `cmake/auv3_Info.plist.in`

New file. AUv3 Info.plist template with `NSExtension` structure:

```xml
<key>NSExtension</key>
<dict>
  <key>NSExtensionPointIdentifier</key>
  <string>com.apple.AudioUnit-UI</string>
  <key>NSExtensionPrincipalClass</key>
  <string>${AUV3_PRINCIPAL_CLASS}</string>
  <key>NSExtensionAttributes</key>
  <dict>
    <key>AudioComponents</key>
    <array>
      <!-- Generated per-plugin entries -->
    </array>
  </dict>
</dict>
```

Each AudioComponent entry contains: `name`, `description`, `manufacturer`, `type`, `subtype`, `version`, `sandboxSafe`, `tags`.

### 1.3 Create build-helper for AUv3

New directory: `src/detail/auv3/build-helper/`
New file: `src/detail/auv3/build-helper/build-helper.cpp`

Reuses the same pattern as `src/detail/auv2/build-helper/build-helper.cpp`:
- Loads the CLAP library, queries plugin factory
- Determines AU type from CLAP features (or from `clap_plugin_factory_as_auv2` extension -- reuse the same extension for AUv3 since the type/subtype mapping is identical)
- Generates:
  1. `auv3_Info.plist` -- complete plist with NSExtension and AudioComponents
  2. `generated_auv3_entrypoints.hxx` -- unique Objective-C factory class per plugin

The AUv2 build-helper code for CLAP library loading and metadata extraction can be factored into shared code or duplicated (it's ~200 lines of straightforward logic).

---

## Phase 2: Core Audio Unit Class (Audio Only, No GUI)

**Goal**: A functional AUv3 that loads a CLAP plugin, processes audio, but has no parameters or GUI yet.

### 2.1 Create the C++ implementation bridge

New file: `src/detail/auv3/auv3_base.h`

Define a C++ struct that implements the host interfaces:

```cpp
namespace Clap::AUv3 {

struct AUv3ImplDetail : public Clap::IHost,
                        public Clap::IAutomation,
                        public os::IPlugObject {
    std::shared_ptr<Clap::Plugin> plugin;
    std::unique_ptr<ProcessAdapter> processAdapter;
    // ... parameter maps, state ...

    // IHost overrides
    // IAutomation overrides
    // IPlugObject::onIdle() override
};

} // namespace
```

This follows the same pattern as `WrapAsAUV2` in `src/detail/auv2/auv2_base_classes.h` (which inherits from `ausdk::AUBase + IHost + IAutomation + IPlugObject`), except the AUBase part is handled by the Objective-C `AUAudioUnit` subclass.

### 2.2 Create the AUAudioUnit subclass

New file: `src/detail/auv3/auv3_audiounit.h` (Objective-C++ header)
New file: `src/detail/auv3/auv3_audiounit.mm` (Objective-C++ implementation)

```objc
@interface ClapAUv3AudioUnit : AUAudioUnit
- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError **)outError
                                    clapName:(NSString *)clapName
                                      clapId:(NSString *)clapId
                                   clapIndex:(int)clapIndex;
@end
```

The class owns an `AUv3ImplDetail` instance (via `std::unique_ptr` in an ivar or C++ member in the `.mm` file).

Key AUAudioUnit overrides to implement:

| Override | Purpose | CLAP mapping |
|----------|---------|--------------|
| `initWithComponentDescription:options:error:` | Create CLAP plugin instance | `Clap::Plugin::createInstance()` |
| `allocateRenderResourcesAndReturnError:` | Activate plugin, create process adapter | `plugin->activate()`, `plugin->start_processing()` |
| `deallocateRenderResources` | Deactivate plugin | `plugin->stop_processing()`, `plugin->deactivate()` |
| `internalRenderBlock` | Return the audio render block | Calls `processAdapter->process()` |
| `inputBusses` / `outputBusses` | Audio bus arrays | From `clap_plugin_audio_ports_t` |
| `latency` | Report latency in seconds | From `clap_plugin_latency_t` |
| `tailTime` | Report tail time in seconds | From `clap_plugin_tail_t` |
| `channelCapabilities` | Supported I/O configurations | From audio port channel counts |
| `parameterTree` | Parameter tree (Phase 3) | From `clap_plugin_params_t` |
| `fullState` / `setFullState:` | State save/restore (Phase 4) | Via `Clap::StateMemento` |

### 2.3 Create the AUAudioUnitFactory

New file: `src/detail/auv3/auv3_factory.h`
New file: `src/detail/auv3/auv3_factory.mm`

```objc
@interface ClapAUv3Factory : NSObject <AUAudioUnitFactory>
- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error;
@end
```

The build-helper generates unique factory subclass names per plugin (e.g., `ClapAUv3Factory_inst0`), each hardcoding the CLAP plugin ID and index. This mirrors the AUv2 pattern in `generated_entrypoints.hxx`.

### 2.4 Audio bus setup

In `allocateRenderResourcesAndReturnError:`:
- Query `clap_plugin_audio_ports_t::count()` and `::get()` for input/output ports
- Create `AUAudioUnitBus` objects with matching `AVAudioFormat` (channel count, sample rate)
- Wrap in `AUAudioUnitBusArray` for `inputBusses` / `outputBusses` properties
- Set the format from the host's negotiated format

### 2.5 Create the AUv3 ProcessAdapter

New file: `src/detail/auv3/process.h`
New file: `src/detail/auv3/process.mm`

The render block receives:
- `AudioBufferList *outputData` -- output audio buffers
- `AURenderPullInputBlock pullInputBlock` -- function to pull input audio
- `const AURenderEvent *realtimeEventListHead` -- linked list of parameter + MIDI events
- `AVAudioFrameCount frameCount` -- number of frames to process
- `const AudioTimeStamp *timestamp` -- timing info

The ProcessAdapter translates this to CLAP's `clap_process_t`:

1. **Pull input audio** via `pullInputBlock` into internal `AudioBufferList`
2. **Walk `AURenderEvent` linked list**, translating:
   - `AURenderEventParameter` / `AURenderEventParameterRamp` -> `clap_event_param_value_t`
   - `AURenderEventMIDI` -> `clap_event_midi_t` or `clap_event_note_on/off_t` (based on dialect preference)
   - `AURenderEventMIDIEventList` -> MIDI 2.0 events (future)
   - `AURenderEventMIDISysEx` -> `clap_event_midi_sysex_t`
3. **Construct `clap_process_t`** with audio buffers, input events, transport info
4. **Call `plugin->_plugin->process(&process_data)`**
5. **Process output events** from CLAP (parameter changes, MIDI output)
6. **Copy output audio** to `outputData`

**Reusable from AUv2 ProcessAdapter** (`src/detail/auv2/process.h/cpp`):
- `doubleToBeatTime()` / `doubleToSecTime()` utility functions -- move to a shared header or duplicate
- `clap_multi_event_t` union type definition
- Event sorting logic (`sortEventIndices`)
- Output event processing pattern (`enqueueOutputEvent`)
- Active note tracking logic
- Transport state flag mapping (conceptually identical, the source data format differs slightly)

**New for AUv3**:
- `AURenderEvent` linked list traversal (replaces explicit `addMIDIEvent`/`addParameterEvent` calls)
- Input pulling via `AURenderPullInputBlock` (replaces `ausdk::AUInputElement::PullInput`)
- Transport info retrieval via `AUAudioUnit.transportStateBlock` (replaces AUv2 host callbacks)

### 2.6 Main wrapper glue

New file: `src/wrapasauv3.mm`

This file:
- Implements the `IHost` methods that bridge AUv3 host callbacks to/from CLAP
- Handles CLAP library loading (same pattern as `wrapasauv2.cpp` lines 113-154)
- Registers with `os::attach()` for idle callbacks

Most `IHost` method implementations follow the same logic as in `wrapasauv2.cpp`, adapted for AUv3 APIs:
- `latency_changed()` -> post `kAudioUnitProperty_Latency` change notification
- `mark_dirty()` -> no direct AUv2 equivalent, but AUv3 handles this via state observation
- `request_callback()` -> dispatch to main thread via `dispatch_async`
- `setupAudioBusses()` -> build `AUAudioUnitBusArray`
- `setupMIDIBusses()` -> set `MIDIOutputNames` property
- `setupParameters()` -> build `AUParameterTree` (Phase 3)

---

## Phase 3: Parameter System

**Goal**: Full parameter tree with automation support.

### 3.1 Create parameter bridge

New file: `src/detail/auv3/auv3_parameters.h`
New file: `src/detail/auv3/auv3_parameters.mm`

Build `AUParameterTree` from CLAP parameters:

1. Iterate all CLAP params via `params->count()` / `params->get_info()`
2. For each param, create an `AUParameter`:
   - `address` = `clap_param_info_t.id` (AUParameterAddress is uint64_t, same as clap_id)
   - `displayName` = `clap_param_info_t.name`
   - `min`/`max` = `clap_param_info_t.min_value`/`max_value`
   - `unit` mapping:
     - `CLAP_PARAM_IS_STEPPED` with range 0..1 -> `kAudioUnitParameterUnit_Boolean`
     - `CLAP_PARAM_IS_STEPPED` -> `kAudioUnitParameterUnit_Indexed`
     - Otherwise -> `kAudioUnitParameterUnit_Generic`
   - `flags`: `kAudioUnitParameterFlag_IsReadable`, `kAudioUnitParameterFlag_IsWritable` (unless readonly/hidden)
3. Group by `clap_param_info_t.module` path:
   - Split module string on `/`
   - Create `AUParameterGroup` hierarchy
   - This replaces AUv2's crude "clumps" mechanism with proper nesting
4. Create `AUParameterTree` with top-level children

### 3.2 Wire parameter observation blocks

On the `AUParameterTree`:

```objc
tree.implementorValueObserver = ^(AUParameter *param, AUValue value) {
    // Host changed a parameter -> queue CLAP_EVENT_PARAM_VALUE for process
};

tree.implementorValueProvider = ^AUValue(AUParameter *param) {
    // Host reads current value
    double v;
    ext_params->get_value(plugin, param.address, &v);
    return (AUValue)v;
};

tree.implementorStringFromValueCallback = ^NSString*(AUParameter *param, const AUValue *value) {
    char buf[256];
    ext_params->value_to_text(plugin, param.address, *value, buf, sizeof(buf));
    return [NSString stringWithUTF8String:buf];
};

tree.implementorValueFromStringCallback = ^AUValue(AUParameter *param, NSString *string) {
    double v;
    ext_params->text_to_value(plugin, param.address, [string UTF8String], &v);
    return (AUValue)v;
};
```

### 3.3 Parameter automation (CLAP -> Host)

When the CLAP plugin changes a parameter (via `IAutomation::onPerformEdit`), the wrapper must notify the host. In AUv3, this is done by setting `AUParameter.value` which triggers KVO.

Use the same `fixedqueue` pattern from AUv2 (`src/detail/shared/fixedqueue.h`) to safely pass events from the audio thread to the main thread, where `onIdle()` processes them and updates `AUParameter.value`.

### 3.4 Parameter events in render block

In the process adapter, `AURenderEventParameter` events from the linked list are translated to `clap_event_param_value_t` using the `AUParameter.address` (which equals `clap_id`). `AURenderEventParameterRamp` can be converted to a value at the event's sample offset.

### 3.5 Parameter ordering extension

The existing `CLAP_PLUGIN_AUV2_PARAM_ORDERING` extension can be reused for AUv3 to control the order parameters appear in the tree. This matters for Logic Pro compatibility. Consider also defining a `CLAP_PLUGIN_AUV3_PARAM_ORDERING` alias or just reusing the AUv2 one (since the concept is identical).

---

## Phase 4: State Save/Restore

**Goal**: Presets and session state persist correctly.

### 4.1 Implement `fullState` property

```objc
- (NSDictionary<NSString *, id> *)fullState {
    NSMutableDictionary *state = [[super fullState] mutableCopy] ?: [NSMutableDictionary new];

    Clap::StateMemento chunk;
    plugin->_ext._state->save(plugin->_plugin, chunk);

    state[@"clapState"] = [NSData dataWithBytes:chunk.data() length:chunk.size()];
    return state;
}
```

### 4.2 Implement `setFullState:`

```objc
- (void)setFullState:(NSDictionary<NSString *, id> *)fullState {
    [super setFullState:fullState];

    NSData *clapState = fullState[@"clapState"];
    if (clapState) {
        Clap::StateMemento chunk;
        chunk.setData((const uint8_t *)[clapState bytes], [clapState length]);
        plugin->_ext._state->load(plugin->_plugin, chunk);
    }
}
```

This directly mirrors the AUv2 pattern in `wrapasauv2.cpp` lines 1198-1333 but uses `NSDictionary` instead of `CFDictionary`.

### 4.3 Preset support

Implement `factoryPresets` and `currentPreset` properties if the CLAP plugin provides factory presets. This is optional for the initial implementation.

---

## Phase 5: MIDI

**Goal**: MIDI input and output work for instruments and effects.

### 5.1 MIDI input

Already partially handled in Phase 2 (the render block receives `AURenderEventMIDI` events). Complete the implementation:

- Parse MIDI status bytes to determine note on/off vs. CC vs. other
- Check `clap_note_port_info_t.preferred_dialect`:
  - `CLAP_NOTE_DIALECT_CLAP` -> convert to `clap_event_note_on/off_t`
  - `CLAP_NOTE_DIALECT_MIDI` -> pass as `clap_event_midi_t`
- Handle `AURenderEventMIDIEventList` for MIDI 2.0 (macOS 12+, lower priority)

### 5.2 MIDI output

- Set `MIDIOutputNames` property from CLAP note port names (`clap_plugin_note_ports_t`)
- When CLAP plugin produces note/MIDI output events in `process()`, call:
  ```objc
  if (self.MIDIOutputEventBlock) {
      self.MIDIOutputEventBlock(timestamp, cable, length, midiBytes);
  }
  ```
- This replaces AUv2's `MIDIPacketList` + `AUMIDIOutputCallbackStruct` mechanism

---

## Phase 6: GUI

**Goal**: CLAP plugin GUI displays in the host's AU window.

### 6.1 Create AUViewController subclass

New file: `src/detail/auv3/auv3_viewcontroller.h`
New file: `src/detail/auv3/auv3_viewcontroller.mm`

```objc
@interface ClapAUv3ViewController : AUViewController
@property (nonatomic, weak) ClapAUv3AudioUnit *audioUnit;
@end
```

In `viewDidLoad`:
1. Query CLAP `gui->is_api_supported(CLAP_WINDOW_API_COCOA)` 
2. Call `gui->create()`, `gui->get_size()` to determine dimensions
3. Set `self.preferredContentSize`
4. Call `gui->set_parent()` with `self.view` as the NSView
5. Set up a timer for `gui->on_timer()` if the CLAP plugin uses timers

In `viewDidDisappear`:
1. Call `gui->destroy()`
2. Invalidate timers

### 6.2 Wire up view controller request

In the AUAudioUnit subclass:
```objc
- (void)requestViewControllerWithCompletionHandler:(void (^)(AUViewControllerBase *))completionHandler {
    ClapAUv3ViewController *vc = [[ClapAUv3ViewController alloc] init];
    vc.audioUnit = self;
    completionHandler(vc);
}
```

### 6.3 Resize support

Implement `gui_request_resize` in `IHost`:
- Update `preferredContentSize` on the view controller
- The host will respond to the size change

This is simpler than AUv2 where resize required the `ui_connection` struct and custom property mechanism.

---

## Phase 7: Build System Integration

**Goal**: AUv3 is a first-class format in the build system.

### 7.1 Add to `make_clapfirst_plugins()`

File: `cmake/make_clapfirst.cmake`

- Add `AUV3` to the `PLUGIN_FORMATS` list parsing (alongside CLAP, VST3, AUV2, AAX, WCLAP)
- Add `list(FIND C1ST_PLUGIN_FORMATS "AUV3" BUILD_AUV3)` 
- Add AUv3 block (guarded by `APPLE AND ${BUILD_AUV3} GREATER -1`):

```cmake
if (APPLE AND ${BUILD_AUV3} GREATER -1)
    set(AUV3_TARGET ${C1ST_TARGET_NAME}_auv3)
    add_library(${AUV3_TARGET} MODULE)
    target_sources(${AUV3_TARGET} PRIVATE ${C1ST_ENTRY_SOURCE})
    target_link_libraries(${AUV3_TARGET} PRIVATE ${C1ST_IMPL_TARGET})
    target_add_auv3_wrapper(TARGET ${AUV3_TARGET} ...)
    add_dependencies(${ALL_TARGET} ${AUV3_TARGET})
endif()
```

### 7.2 Add to `top_level_default.cmake`

Add option `CLAP_WRAPPER_BUILD_AUV3` and conditional AUv3 target creation, mirroring the AUv2 block.

### 7.3 Add to test example

File: `tests/clap-first-example/CMakeLists.txt`

Add `AUV3` to the `PLUGIN_FORMATS` list to test the build.

### 7.4 Copy-after-build support

In `shared_prologue.cmake`, extend `target_copy_after_build()` to handle `.appex`:
- macOS install path: `~/Library/Audio/Plug-Ins/Components/` (same as AUv2, the system discovers AUv3 from here too for in-process)

---

## Phase 8: Packaging Decisions

### 8.1 In-process AUv3 (macOS, Phase 1 target)

For macOS, AUv3 can run **in-process** (loaded directly into the host like AUv2). This is the simplest path:
- The `.appex` bundle is registered and discovered by the system
- No separate host app required for development/testing
- CLAP library loading works identically to AUv2 (same process space)
- Use `pluginkit -a path/to/Plugin.appex` to register during development

Bundle structure:
```
PluginName.appex/
  Contents/
    Info.plist
    MacOS/
      PluginName
    Resources/
      ...
    PlugIns/          (optional: embedded CLAP)
      PluginName.clap/
```

### 8.2 Containing host app (deferred)

For App Store distribution or iOS, a containing app is required. This can be added as an optional `target_add_auv3_host_app()` CMake function later. The host app would be a minimal macOS/iOS app that:
- Registers the embedded `.appex`
- Optionally provides a standalone UI

### 8.3 iOS considerations (deferred but architecturally prepared)

- Use `#if TARGET_OS_IPHONE` / `#if TARGET_OS_OSX` guards where platform-specific code exists
- GUI: `NSViewController` (macOS) vs `UIViewController` (iOS) -- the `AUViewController` base class handles this
- CLAP window API: `CLAP_WINDOW_API_COCOA` (macOS) -- iOS would need investigation (likely same since it's an NSView/UIView question)
- Out-of-process hosting on iOS introduces XPC complexity -- defer entirely

---

## Phase 9: Polish and Edge Cases

### 9.1 `param_rescan` support
When the CLAP plugin requests a parameter rescan, rebuild the `AUParameterTree`. This is complex and can initially log a warning.

### 9.2 `request_callback` / `on_main_thread`
Use `dispatch_async(dispatch_get_main_queue(), ...)` to schedule callbacks on the main thread. This is cleaner than AUv2's `CFRunLoopTimer` approach.

### 9.3 `latency_changed` / `tail_changed`
Post `kAudioUnitProperty_Latency` / `kAudioUnitProperty_TailTime` change notifications so the host picks up the new values.

### 9.4 Optional: `include/clapwrapper/auv3.h`
If AUv3-specific CLAP extensions are needed (beyond what `auv2.h` already provides), create this header. Initially, the `auv2.h` extensions (factory info, parameter ordering) should be reusable for AUv3.

---

## New Files Summary

```
cmake/wrap_auv3.cmake                              -- CMake wrapper function
cmake/auv3_Info.plist.in                            -- Info.plist template

src/wrapasauv3.mm                                   -- Main glue (IHost impl)

src/detail/auv3/auv3_base.h                         -- C++ bridge struct (IHost + IAutomation)
src/detail/auv3/auv3_audiounit.h                    -- AUAudioUnit subclass @interface
src/detail/auv3/auv3_audiounit.mm                   -- AUAudioUnit subclass @implementation
src/detail/auv3/auv3_factory.h                      -- AUAudioUnitFactory @interface
src/detail/auv3/auv3_factory.mm                     -- AUAudioUnitFactory @implementation
src/detail/auv3/auv3_parameters.h                   -- Parameter tree builder
src/detail/auv3/auv3_parameters.mm                  -- Parameter tree implementation
src/detail/auv3/process.h                           -- AUv3 ProcessAdapter
src/detail/auv3/process.mm                          -- Render block + event translation
src/detail/auv3/auv3_viewcontroller.h               -- AUViewController subclass
src/detail/auv3/auv3_viewcontroller.mm              -- GUI hosting
src/detail/auv3/build-helper/build-helper.cpp       -- Metadata -> plist/entrypoint generator
```

## Files to Modify

```
cmake/wrapper_functions.cmake                       -- Add include(cmake/wrap_auv3.cmake)
cmake/make_clapfirst.cmake                          -- Add AUV3 format support
cmake/top_level_default.cmake                       -- Add AUV3 build option
cmake/shared_prologue.cmake                         -- Extend copy-after-build for .appex
tests/clap-first-example/CMakeLists.txt             -- Add AUV3 to test formats
```

## Reusable Existing Code

| Component | Location | Reuse |
|-----------|----------|-------|
| `Clap::Plugin` proxy | `src/clap_proxy.h/cpp` | Direct use (no changes) |
| `Clap::IHost` interface | `src/clap_proxy.h:36-94` | Implement all methods |
| `Clap::IAutomation` interface | `src/detail/clap/automation.h` | Implement 3 methods |
| `Clap::StateMemento` | `src/clap_proxy.h:275-311` | Direct use for state save/load |
| `os::IPlugObject` | `src/detail/os/osutil.h` | Implement `onIdle()` |
| `os::attach/detach` | `src/detail/os/macos.mm` | Direct use for timer registration |
| `fixedqueue` | `src/detail/shared/fixedqueue.h` | Audio->UI parameter event queue |
| CLAP library loading | `src/detail/clap/fsutil.h` | Same loading pattern |
| `clap_plugin_factory_as_auv2` | `include/clapwrapper/auv2.h` | Reuse for type/subtype info |
| Build-helper CLAP metadata extraction | `src/detail/auv2/build-helper/` | Same logic, different output format |

## Implementation Order (Recommended)

1. **Phase 1** -- Build system skeleton (cmake, plist template, build-helper stub)
2. **Phase 2** -- Core audio unit (factory, AUAudioUnit subclass, process adapter, audio only)
3. **Phase 3** -- Parameters (AUParameterTree, observation, automation)
4. **Phase 5** -- MIDI (input parsing in render block, output via MIDIOutputEventBlock)
5. **Phase 4** -- State save/restore (fullState property)
6. **Phase 6** -- GUI (AUViewController, CLAP GUI hosting)
7. **Phase 7** -- Build system integration (clapfirst, tests)
8. **Phase 8** -- Packaging refinement
9. **Phase 9** -- Polish

## Verification

After each phase, verify by:
1. **Build**: `cmake --build . --target <plugin>_auv3` succeeds
2. **auval**: `auval -v <type> <subtype> <mfgr>` passes basic validation (after Phase 2+)
3. **DAW test**: Load in Logic Pro or GarageBand, verify audio/params/state/GUI (progressively)
4. **Existing formats**: Ensure VST3/AUv2/AAX/Standalone still build and work (regression check)
