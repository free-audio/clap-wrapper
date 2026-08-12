*"Wrappers to run your CLAP in other plugin formats instantly."*

# The clap-wrapper Project

The `clap-wrapper` project allows you to wrap your CLAP plugin
into other plugin or standalone executable formats, letting your
plugin run in hosts which don't support the CLAP format and 
allowing you to distribute standalone executable versions of your 
CLAP.

Our [wiki](https://github.com/free-audio/clap-wrapper/wiki) is the root of our developer documentation.

Currently the `clap-wrapper` supports projecting a CLAP into

- VST3
- Audio Unit v2 (AUv2)
- Audio Unit v3 (AUv3, on macOS and iOS)
- AAX
- A Simple Standalone

The `clap-wrapper` also provides a variety of deployment,
linkage and build time options to CLAP developers.

## The Core Idea

The core idea of `clap-wrapper` is as follows

1. The `clap-wrapper` is an implementation of a clap host which
will load your CLAP using a variety of techniques.
2. The `clap-wrapper` provides an implementation of a plugin format
or standalone executable which implements the format to the host
or OS, but uses that host to load your clap.

And with that, voila, your CLAP can appear in a VST3, AUv2 or AUv3
host or can appear to be a self contained standalone executable.
Available features in CLAP are transposed to equivalent features
in the target format.

## Getting Started

Our [wiki](https://github.com/free-audio/clap-wrapper/wiki) is the root of our developer documentation.
Since building a `clap-wrapper` involves several choices we
strongly suggest you consult it first.

The simplest form of a clap wrapper, though, is a VST3
which dynamically loads a CLAP of the same name using your
copy of the VST3 and CLAP SDK. To build this, you can use the
simple CMake command

```bash
git clone https://github.com/free-audio/clap-wrapper.git
mkdir build
cmake -B build \ 
      -DCLAP_SDK_ROOT={path to clap sdk} \
      -DVST3_SDK_ROOT={path to vst3 sdk} \
      -DCLAP_WRAPPER_OUTPUT_NAME="The Name of your CLAP"
```
If you'd like to also build for AUv2 include `-DCLAP_WRAPPER_BUILD_AUV2=ON` and the SDK in the folder `AudioUnitSDK`.

To build an AUv3 app extension include `-DCLAP_WRAPPER_BUILD_AUV3=ON`.
AUv3 builds require the Xcode generator (`-G Xcode`) — the `.appex` is
linked as an app-extension product and signed by Xcode; other generators
stop at configure time with an explanatory error. In the clap-first flow,
add `AUV3` to `PLUGIN_FORMATS` instead. See the wiki for the current AUv3
feature status.

On **iOS** the AUv3 app extension is the only available format, and it
places two requirements on your CLAP:

- it must be linked **statically** into the appex (iOS app extensions
  cannot load a `.clap` at runtime), so the clap-first layout is required
  and one appex hosts exactly one plugin;
- its GUI must support the `CLAP_WINDOW_API_UIKIT` (`"uikit"`) window API
  and attach to the `UIView` it is given — there is no `NSView` on iOS.
  That constant needs CLAP 1.2.8 or newer.

The appex ships inside a containing app, and for an instrument that app
is a complete standalone synth you can release.

See [docs/ios.md](docs/ios.md) for the full iOS instructions.

## Licensing

The `clap-wrapper` project is released under the MIT license.
The SDKs and libraries it references remain under their respective
licenses:

| SDK / Library | Used for | License |
|---|---|---|
| [CLAP SDK](https://github.com/free-audio/clap) | all wrappers | MIT |
| [VST3 SDK](https://github.com/steinbergmedia/vst3sdk) (Steinberg) | VST3 wrapper | MIT (use of the VST trademark/logo per Steinberg's guidelines) |
| [AudioUnitSDK](https://github.com/apple/AudioUnitSDK) (Apple) | AUv2 wrapper | Apache 2.0 |
| AAX SDK (Avid) | AAX wrapper | GPL3 **or** commercial Avid AAX SDK License Agreement |
| [RtAudio](https://github.com/thestk/rtaudio) | standalone audio I/O | MIT-like (RtAudio license) |
| [RtMidi](https://github.com/thestk/rtmidi) | standalone MIDI I/O | MIT-like (RtMidi license) |
| [WIL](https://github.com/microsoft/wil) (Microsoft) | standalone, Windows only | MIT |
| [gulrak/filesystem](https://github.com/gulrak/filesystem) | std::filesystem fallback on older toolchains | MIT |
| [{fmt}](https://github.com/fmtlib/fmt) (vendored in `libs/fmt`) | string formatting | MIT |
| [PreSonus plugin extensions](https://github.com/fenderdigital/presonus-plugin-extension) (vendored in `libs/psl`) | gain-reduction extension in VST3 | Public domain |

The AUv3 wrapper uses only Apple system frameworks (AudioToolbox,
AVFoundation, CoreAudioKit) and needs no additional SDK.

Please note that AAX is dual-licensed: the Avid AAX SDK is available
either under the GPL3 or under the commercial
[Avid AAX SDK License Agreement](https://developer.avid.com/aax). So
to wrap an AAX plugin you must either release it under the GPL3 or
agree to Avid's SDK license — and joining the Avid developer program
is required in practice anyway, since AAX binaries must be signed
with Avid/PACE tooling to load in commercial Pro Tools releases.

