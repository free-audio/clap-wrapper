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

`target_add_auv3_wrapper` also takes:

- `INFO_PLIST_ADDITIONS <file.plist>` — keys merged into the generated
  Info.plist of the appex. Dictionaries are merged key by key, anything else
  (strings, numbers, arrays) replaces the generated value.
  (`AUV3_INFO_PLIST_ADDITIONS` in `make_clapfirst_plugins`.)
- `ENTITLEMENTS <file.entitlements>` — the appex's entitlements, e.g. an App
  Group shared with your app. (`AUV3_ENTITLEMENTS` in `make_clapfirst_plugins`.)
- `CODE_SIGN_IDENTITY` / `DEVELOPMENT_TEAM` — let Xcode sign the appex with
  your team instead of ad hoc. They default to
  `CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY` / `CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM`;
  with neither set the appex is signed ad hoc as before.

`target_embed_auv3_appex(TARGET <your app> AUV3_TARGET <appex target>)`
embeds the appex in an application bundle of your own. The appex's bundle
identifier has to be the app's identifier plus a suffix.

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

On **WebAssembly** nothing is wrapped at all. Rather than presenting your CLAP
to a host as some other format, the build compiles the CLAP itself into a
`.wclap` — a WebAssembly CLAP — so VST3, AUv2, AUv3, AAX and standalone are all
unavailable there and `PLUGIN_FORMATS` defaults to `WCLAP` on its own. Either
Emscripten or the WASI-SDK will do; the toolchain file you configure with picks
one, and the build detects it from `CMAKE_SYSTEM_NAME` or a `wasm32`/`wasm64`
processor. The module is compiled with SIMD (`-msimd128`) and exports
`clap_entry` and `malloc` for the web host to call.

The layout depends on whether you pass a `RESOURCE_DIRECTORY`. With one, you get
a bundle — `YourPlugin.wclap/module.wasm` beside a copy of that directory, and a
`.tar.gz` of the whole bundle for shipping. Without one, a single flat
`YourPlugin.wclap.wasm`.

A **standalone** can be opened from a link, say a licence activation link on a
shop page: give it url schemes with `URL_SCHEMES` (`STANDALONE_URL_SCHEMES` in
`make_clapfirst_plugins`), and a link such as `yourplugin://activate?code=1234`
launches the standalone, or brings it to the front if it runs, and hands the
url to the plugin. The plugin takes it with `open_url` on its side of the
standalone features extension (`clapwrapper/standalone_features.h`), once it
exists and its GUI is shown. A url comes from whatever page the link is on, so
treat it as untrusted input. What the system needs to know about the schemes:

- **macOS** reads them from the bundle's Info.plist, which the build writes.
  The system registers an app when it is installed or first launched; for a
  build that is neither, `lsregister -f "Your Plugin.app"` (in
  `/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/`)
  makes it the handler.
- **Windows** needs registry keys, which the installer writes, per machine
  under `HKLM\Software\Classes` or per user under `HKCU\Software\Classes`:

  ```
  [HKEY_CURRENT_USER\Software\Classes\yourplugin]
  @="URL:Your Plugin"
  "URL Protocol"=""

  [HKEY_CURRENT_USER\Software\Classes\yourplugin\shell\open\command]
  @="\"C:\\Program Files\\Your Plugin\\Your Plugin.exe\" \"%1\""
  ```

  A link launches the standalone with the url as its argument. When an instance
  is already running, the new process hands the url to it and quits; with more
  than one running, one of them takes it. A launch without a url starts another
  instance, as before.
- **Linux** takes the scheme from the `.desktop` file the installer writes:
  `MimeType=x-scheme-handler/yourplugin;` and `Exec=... %u`. The url arrives as
  the argument of a new instance; one already running does not hear of it.

## Contribution

Development happens on `next`; `main` only moves at a release. Before opening an
issue or a pull request:

- Check whether the problem is already fixed on `next`. Issues are not closed
  automatically when a fix lands, so an open issue is not proof of an unfixed bug.
- Base pull requests on `next`, not `main`.
- Make sure your commit is clang-formatted according to the `.clang-format` file.

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
| [RtAudio](https://github.com/thestk/rtaudio) | standalone audio I/O on Linux, on macOS and Windows only with `CLAP_WRAPPER_STANDALONE_RTAUDIO=ON` or (Windows) `RTAUDIO_API_ASIO=ON` | MIT-like (RtAudio license) |
| [RtMidi](https://github.com/thestk/rtmidi) | standalone MIDI I/O on Linux, on macOS and Windows only with `CLAP_WRAPPER_STANDALONE_RTAUDIO=ON` | MIT-like (RtMidi license) |
| [ASIO SDK](https://www.steinberg.net/developers/) (Steinberg), as bundled by RtAudio | standalone ASIO on Windows, only when built with `RTAUDIO_API_ASIO=ON` | GPL3 **or** Steinberg's proprietary ASIO SDK License Agreement |
| [WIL](https://github.com/microsoft/wil) (Microsoft) | standalone, Windows only | MIT |
| [gulrak/filesystem](https://github.com/gulrak/filesystem) | std::filesystem fallback on older toolchains | MIT |
| [{fmt}](https://github.com/fmtlib/fmt) (vendored in `libs/fmt`) | string formatting | MIT |
| [PreSonus plugin extensions](https://github.com/fenderdigital/presonus-plugin-extension) (vendored in `libs/psl`) | gain-reduction extension in VST3 | Public domain |

On macOS and Windows the standalone uses the platform's own audio and MIDI APIs
(CoreAudio and CoreMIDI, WASAPI and WinMM) and fetches neither RtAudio nor RtMidi.
`-DCLAP_WRAPPER_STANDALONE_RTAUDIO=ON` builds it on RtAudio and RtMidi instead, as
Linux always does. On Windows, `-DRTAUDIO_API_ASIO=ON` adds ASIO, taken from
RtAudio, next to WASAPI; the audio/MIDI settings window then offers the selected
driver's control panel. Like AAX below, ASIO is
dual-licensed: a standalone built with it is either released under the GPL3 or
covered by Steinberg's ASIO SDK License Agreement.

The AUv3 wrapper uses only Apple system frameworks (AudioToolbox,
AVFoundation, CoreAudioKit) and needs no additional SDK.

Please note that AAX is dual-licensed: the Avid AAX SDK is available
either under the GPL3 or under the commercial
[Avid AAX SDK License Agreement](https://developer.avid.com/aax). So
to wrap an AAX plugin you must either release it under the GPL3 or
agree to Avid's SDK license — and joining the Avid developer program
is required in practice anyway, since AAX binaries must be signed
with Avid/PACE tooling to load in commercial Pro Tools releases.

