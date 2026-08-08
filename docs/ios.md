# How to use the clap-wrapper on iOS

On iOS the wrapper produces exactly one plugin format: an **AUv3 app
extension** (`.appex`), together with the **app that contains it**. That
containing app is not just a test harness - for an instrument it is a
complete standalone synth app (audio out, MIDI in and out, your plugin
UI) that you can ship.

VST3, AUv2, AAX and the RtAudio standalone do not exist on iOS. Do not put
them into `PLUGIN_FORMATS`.

## Requirements

- macOS with Xcode (a real Xcode install, not just command line tools)
- CMake 3.21+
- **Xcode generator** (`-G Xcode`). Ninja/Makefiles cannot produce a
  loadable appex; the wrapper stops at configure time if you try.
- Clap-first layout: your CLAP must be available as a **static library**
  plus a small entry source (see `tests/clap-first-example`).
  On iOS there is no other option (see "Static linking" below).
- An Apple Developer team ID if you want to run on a real device.
  The simulator works without one.

## The five things that are different from macOS and Windows

**1. No dynamic loading. Ever.**
On macOS/Windows the wrapper opens your `.clap` at runtime (`dlopen` /
`LoadLibrary`) from a search path. iOS forbids an app extension from
loading code that was not signed into its own bundle, and there is no
user-writable plugin folder. Therefore the CLAP entry point is **linked
statically into the appex binary**. The wrapper is compiled with
`STATICALLY_LINKED_CLAP_ENTRY=1` and calls your `clap_entry` global
directly, skipping all filesystem search.

Consequence: one appex hosts exactly **one** CLAP, and it is the CLAP you
compiled it with. Users cannot install plugins into your app.

**2. A plugin is not a file you copy - it is an app.**
There is no `~/Library/Audio/Plug-Ins` and no `C:\Program Files\Common
Files\VST3`. An `.appex` cannot be installed on its own; it must live
inside a containing `.app` that is installed from the App Store or via
Xcode/TestFlight. The system registers the extension when the app is
installed. Shipping a plugin on iOS means shipping an app.

Bundle layout also differs: iOS bundles are flat. The extension goes to
`MyApp.app/PlugIns/MyPlugin.appex`, not `Contents/PlugIns/`.

**3. Bundle IDs and signing are enforced, not cosmetic.**
- The appex bundle ID **must** be a prefix child of the host app ID,
  e.g. host `com.you.myplug` and appex `com.you.myplug.auv3`. `installd`
  rejects anything else. The wrapper does not rewrite the ID for you,
  because rewriting it invalidates the signed entitlements.
- Host and appex must use the **same** development team, and both must
  target the same device family (the wrapper sets iPhone+iPad, `1,2`, for
  both).
- Device builds need a real signing identity. Simulator builds fall back
  to ad-hoc signing automatically.
- There is **no** app-sandbox entitlement to set. On macOS the AUv3 appex
  must be sandboxed explicitly; on iOS sandboxing is implicit.

**4. The GUI is UIKit, not Cocoa.**
The wrapper asks your plugin for a view with the CLAP window API string
`"uikit"` (`CLAP_WINDOW_API_UIKIT`) instead of `CLAP_WINDOW_API_COCOA`,
and hands you a `UIView` parent instead of an `NSView`.

Your plugin must:
- report `is_api_supported(CLAP_WINDOW_API_UIKIT)` as true,
- accept `CLAP_WINDOW_API_UIKIT` in `gui->create()`,
- treat `clap_window.ptr` as a `UIView` and add its own `UIView` to it.

`CLAP_WINDOW_API_UIKIT` was added to `clap/ext/gui.h` in CLAP 1.2.8, so
iOS builds need a CLAP SDK of at least that version. The wrapper pins
1.2.10 (`cmake/base_sdks.cmake`); if you point `CLAP_SDK_ROOT` at an
older CLAP, the iOS build will not compile.

There are no floating windows on iOS - the editor is always embedded and
is resized by the host.

**5. AU identity must be given in CMake.**
On macOS the wrapper builds a small helper tool that opens your CLAP and
reads the AUv2/AUv3 identity out of it. That helper is a macOS binary and
Xcode cannot build a macOS tool and an iOS appex in one configure run, so
on iOS the wrapper generates `Info.plist` and the factory class directly
from CMake variables. If you do not pass manufacturer/subtype/type, they
silently default to `errr` and your plugin will register with a garbage
identity.

## Configure and build

Simulator:

```
cmake -B build-ios -G Xcode \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0

cmake --build build-ios --config Debug
```

Device:

```
cmake -B build-ios-dev -G Xcode \
      -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT=iphoneos \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
      -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=ABCDE12345

cmake --build build-ios-dev --config Release
```

Always set `CMAKE_OSX_DEPLOYMENT_TARGET` explicitly. If it is undefined,
the wrapper's top level CMakeLists sets the macOS default (10.13), which
is meaningless as an iOS version and ends up in the appex `Info.plist`.

## CMake setup

### Step 1: the appex (clap-first)

```cmake
add_library(myplug-impl STATIC my_plugin.cpp)
target_link_libraries(myplug-impl PUBLIC clap clap-wrapper-extensions)

make_clapfirst_plugins(
        TARGET_NAME myplug
        IMPL_TARGET myplug-impl
        OUTPUT_NAME "My Plug"
        ENTRY_SOURCE "my_plugin_entry.cpp"

        BUNDLE_IDENTIFIER "com.you.myplug"
        BUNDLE_VERSION ${PROJECT_VERSION}

        # On iOS only these two are meaningful
        PLUGIN_FORMATS CLAP AUV3

        # Required on iOS - there is no CLAP probing here
        AUV2_MANUFACTURER_NAME "You Audio"
        AUV2_MANUFACTURER_CODE "YoAu"
        AUV2_SUBTYPE_CODE "MyPl"
        AUV2_INSTRUMENT_TYPE "aumu"   # aumu instrument, aufx effect
)
```

This creates the target `myplug_auv3` with the bundle ID
`com.you.myplug.auv3`. `CLAP` must stay in the list (clap-first requires
it) even though a `.clap` file is not usable on iOS.

### Step 2: the app (standalone and appex container)

On macOS the clap-first flow also builds the AUv3 standalone host. On iOS
it does not, because the app involves signing and branding choices - you
wire it up yourself:

```cmake
add_executable(myplug_ios_host)

# The host instantiates the plugin in-process, so it needs the CLAP
# entry symbol too - add the same entry source and impl library.
target_sources(myplug_ios_host PRIVATE my_plugin_entry.cpp)
target_link_libraries(myplug_ios_host PRIVATE myplug-impl)

target_add_auv3_standalone_ios_wrapper(
        TARGET myplug_ios_host
        AUV3_TARGET myplug_auv3
        OUTPUT_NAME "My Plug"

        # Host ID: the appex ID above must be this plus ".auv3"
        BUNDLE_IDENTIFIER "com.you.myplug"
        BUNDLE_VERSION ${PROJECT_VERSION}

        # Must match the appex identity exactly - the factory class name
        # is derived from manufacturer + subtype
        AU_TYPE "aumu"
        AU_SUBTYPE "MyPl"
        AU_MANUFACTURER "YoAu"

        DEVELOPMENT_TEAM "ABCDE12345"          # optional
        ICON_ASSET_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/Assets.xcassets"
        LAUNCH_SCREEN_IMAGE "LaunchImage"      # optional
)
```

Call order matters: `target_add_auv3_wrapper` (done for you inside
`make_clapfirst_plugins`) must run on the AUv3 target before
`target_add_auv3_standalone_ios_wrapper`.

Optional knobs: `CUSTOM_INFO_PLIST_TEMPLATE` to supply your own
`Info.plist.in`, `EXTRA_INFO_PLIST_ENTRIES` for one-off plist keys,
`APP_ICON_NAME` if your icon set is not called `AppIcon`.

The host embeds the appex into `PlugIns/` and re-signs itself after
build.

## Running

Simulator:

```
xcrun simctl boot "iPhone 15"
xcrun simctl install booted "<path to My Plug.app>"
xcrun simctl launch booted com.you.myplug
```

The Xcode generator puts the app in a per-config, per-platform folder,
e.g. `build-ios/Debug-iphonesimulator/` (plus your project subdirectory
and `ASSET_OUTPUT_DIRECTORY` if you set one).

Device: open the generated Xcode project, select the app target and Run.

Launching the app gives you the standalone. Installing it is also what
registers the AUv3 on the system, so the plugin then shows up in other
hosts - AUM, Cubasis, Loopy Pro, GarageBand.

### What the app does

For an instrument (`aumu` / `aufg`) the app is a usable standalone synth
in its own right, and it is meant to be shipped that way:

- your plugin UI fills the screen, inside the safe area;
- all plugin output busses are mixed into the system output;
- it connects to every MIDI source it finds, including devices plugged in
  after launch, and exposes a virtual MIDI output if your plugin has MIDI
  out ports;
- it survives interruptions and route changes (calls, Siri, AirPods,
  AirPlay, USB audio) and rebuilds the audio graph when the hardware
  sample rate changes;
- background audio is enabled, so it keeps playing when backgrounded;
- app icon, launch screen and extra `Info.plist` keys are all settable
  from CMake (see the knobs above).

What it does not do: audio **input** busses are not wired up, because the
app has no UI for picking an input source. An effect plugin therefore
gets silence - the standalone app is an instrument story only. Effects
still work normally in a real AUv3 host.

### Why the app instantiates the plugin in-process

Since iOS 18, the process-local AudioComponent registry hides third-party
AUv3 extensions from ordinary host processes - including from an app
looking for its *own* embedded appex. So the app does **not** search the
registry. It links the wrapper runtime and the generated factory class
into itself and creates the plugin directly. Your appex is still built,
still installed and still works normally in external hosts.

## Troubleshooting

| Symptom | Cause |
|---|---|
| Configure fails "AUv3 requires the Xcode generator" | Not using `-G Xcode` |
| Plugin registers as "errr" | AU identity not passed to CMake |
| Appex fails to install on device (OSStatus -3000) | Appex bundle ID is not `<host-id>.auv3`, or team mismatch |
| "no code-sign identity for a device build" | Set `DEVELOPMENT_TEAM` or build for the simulator |
| Host shows "Factory class ... is not linked" | Host target missing the entry source / impl library, or AU codes differ between host and appex |
| Plugin appears on iPhone but not on iPad | Device family mismatch; both bundles must be `1,2` |
| GUI stays empty | Plugin does not implement the `CLAP_WINDOW_API_UIKIT` window API |

## Current limitations

- One CLAP per appex, single plugin only.
- `RESOURCE_DIRECTORY` is not supported for AUv3.
- The standalone app has no audio input, so effect plugins are AUv3-only.
- The standalone app sends and receives short MIDI messages, no SysEx.
