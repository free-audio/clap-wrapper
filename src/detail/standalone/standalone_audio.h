#pragma once

/*
 * The standalone's audio, with no platform api in it: the devices a backend lists, and the
 * backend that lists them and runs a stream on them. One backend per audio api. A build
 * has the platform's own - CoreAudio on macOS, WASAPI on Windows - or, built with
 * CLAP_WRAPPER_STANDALONE_RTAUDIO (always on Linux), RtAudio's apis in their place.
 * Windows built with ASIO adds RtAudio's ASIO next to the native WASAPI.
 *
 * Nothing in here throws.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace freeaudio::clap_wrapper::standalone
{
struct AudioDevice
{
  // what the settings and the standalone features extension keep. stable across launches
  // and replugs where the platform has such an id: CoreAudio's device UID, the WASAPI
  // endpoint id. RtAudio's backends use RtAudio's name
  std::string id;

  // for a menu: the device's own name, with no manufacturer in front of it
  std::string name;

  // what RtAudio called this device, so that settings saved by an RtAudio build still find
  // it: "Manufacturer: Name" on macOS, the endpoint's friendly name with WASAPI. empty where
  // RtAudio would not have listed it
  std::string legacyName;

  uint32_t inputChannels{0}, outputChannels{0};

  // one per channel where the device names its channels, empty where it does not;
  // channelName() fills the gaps
  std::vector<std::string> inputChannelNames, outputChannelNames;

  // ascending and without duplicates. a device that reports ranges lists the standard
  // rates inside them (sampleRatesFrom)
  std::vector<uint32_t> sampleRates;

  // the rate the device runs at now; 0 if unknown
  uint32_t currentSampleRate{0};

  // false for a device whose rate is not ours to set, such as a bluetooth device, whose
  // profile sets it: a stream on it runs at currentSampleRate
  bool sampleRateSettable{true};

  // the buffer sizes in frames the device accepts; both 0 where there is no way to tell
  uint32_t minBufferFrames{0}, maxBufferFrames{0};

  bool isDefaultInput{false}, isDefaultOutput{false};

  // a microphone: on macOS the built-in one, a bluetooth device's or an iPhone's; on
  // Windows an endpoint whose form factor is a microphone, headset or handset. one is
  // opened only when the user chose it, never as a default or a fallback: a built-in
  // microphone picks up the speakers beside it, and opening a bluetooth device's input
  // switches the whole device to its hands-free profile, for every application
  bool isMicrophone{false};
  bool isBluetooth{false};

  // hardware an instrument or a line is plugged into (usb, thunderbolt, firewire, pci,
  // avb): what a first run opens in place of a microphone
  bool isInterface{false};

  // devices in one non-zero clock domain run off one clock; 0 is unknown, not shared
  uint32_t clockDomain{0};
};

// the rates offered for a device that reports ranges: RtAudio's table plus 352.8 and
// 384 kHz
const std::vector<uint32_t> &standardSampleRates();

// a device's rate list from the ranges it reports, as CoreAudio does: a range of no width
// is that one rate, any other gives the standard rates inside it. ascending, unique
std::vector<uint32_t> sampleRatesFrom(const std::vector<std::pair<double, double>> &ranges);

// the name of a 0-based channel: the device's own, or "Input 3" / "Output 3"
std::string channelName(const AudioDevice &device, bool input, uint32_t channel);

// the device a saved or requested reference names, among those with channels in that
// direction: by id, then by RtAudio's name (settings from before), then by display name.
// null if none
const AudioDevice *findDevice(const std::vector<AudioDevice> &devices, const std::string &reference,
                              bool input);

// the rates a stream on both could run at: the output's, narrowed to the input's. either
// may be null
std::vector<uint32_t> commonSampleRates(const AudioDevice *output, const AudioDevice *input);

// menu titles for these devices, in their order: the name, with " (2)", " (3)" after
// each repeat of a name
std::vector<std::string> deviceMenuTitles(const std::vector<const AudioDevice *> &devices);

// the input a launch with no saved settings opens in place of the system default: when
// the default input is a microphone, the interface with the most inputs. empty keeps the
// default, which the caller then checks for being a microphone itself
std::string preferredFirstRunInput(const std::vector<AudioDevice> &devices,
                                   const std::string &defaultInput);

struct AudioStreamRequest
{
  // AudioDevice::id; empty opens no such side
  std::string inputDevice, outputDevice;

  // 0-based device channels: the stream opens count channels from the first
  uint32_t inputFirstChannel{0}, inputChannels{0};
  uint32_t outputFirstChannel{0}, outputChannels{0};

  // 0 takes the device's current rate
  uint32_t sampleRate{0};

  // the size to ask the device for; 0 takes the backend's default
  uint32_t bufferFrames{0};
};

// what a started stream was granted, which can differ from the request
struct AudioStreamInfo
{
  uint32_t sampleRate{0};
  uint32_t bufferFrames{0};

  // no process() call ever has more frames than this; the plugin is activated with it
  uint32_t maxFramesPerCallback{0};

  uint32_t inputChannels{0}, outputChannels{0};

  // in frames, from the signal at the device to the buffer and back; 0 if unknown
  uint32_t inputLatency{0}, outputLatency{0};
};

class AudioStreamCallback
{
 public:
  virtual ~AudioStreamCallback() = default;

  // [audio-thread] one pointer per opened channel, in channel order, each holding frames
  // samples of non-interleaved float. every output sample has to be written. inputs is
  // null with no input open, outputs with no output
  virtual void process(const float *const *inputs, uint32_t inputChannels, float *const *outputs,
                       uint32_t outputChannels, uint32_t frames) = 0;
};

enum class AudioBackendEvent
{
  // the running stream stopped on its own: a device went away, or another application
  // changed its format. the message says what
  StreamStopped,

  // the driver wants the stream closed and opened again as it was: an ASIO driver whose
  // own control panel changed the buffer size or the rate. the backend has closed it
  ResetRequested,

  // a device came or went, a default device changed, or a device's rates or channels
  // changed. the host calls refreshDevices() for it, on the main thread. several in a row
  // are taken as one
  DevicesChanged,
};

class AudioBackend
{
 public:
  virtual ~AudioBackend() = default;

  // RtAudio's api names, which settings files and plugins already hold: "core", "wasapi",
  // "asio", "alsa", "pulse", "jack"
  virtual std::string apiId() const = 0;
  virtual std::string apiDisplayName() const = 0;

  // [main-thread] the devices, as listed when first asked and again by the first call
  // after refreshDevices(), never in between: the host holds on to them between its
  // calls. listing may be costly - RtAudio's ASIO loads every driver to do it - so it is
  // not done more often than that
  virtual const std::vector<AudioDevice> &devices() = 0;
  // [main-thread] the host calls it after a DevicesChanged event
  virtual void refreshDevices() = 0;

  // [main-thread] the system's default devices' ids; empty if there is none
  virtual std::string defaultInputDevice() = 0;
  virtual std::string defaultOutputDevice() = 0;

  // [main-thread] stops and closes any stream, then opens this one without starting it,
  // and says what it was granted, so that the plugin can be activated for that before
  // the first process() call. on failure nothing is left open and error says why, in
  // words for the user
  virtual bool open(const AudioStreamRequest &request, AudioStreamInfo &granted, std::string &error) = 0;

  // [main-thread] starts the open stream: process() calls begin. on failure the stream
  // is closed and error says why
  virtual bool start(AudioStreamCallback &callback, std::string &error) = 0;

  // [main-thread] stops and closes the stream, open or running, or one that stopped on
  // its own. no process() call runs once it returns
  virtual void stop() = 0;

  // [main-thread] open() succeeded and stop() has not been called since; also true for a
  // stream that stopped on its own until stop()
  virtual bool isOpen() const = 0;
  // [main-thread] process() calls are being made, or are about to be
  virtual bool isRunning() const = 0;

  // the driver's own settings window, which ASIO drivers have; shown only while a stream
  // is open on the driver
  virtual bool hasControlPanel() const
  {
    return false;
  }
  virtual bool showControlPanel()
  {
    return false;
  }

  // [any-thread] set by the host before anything else is called; called from whichever
  // thread noticed, so its handler hops to the main thread itself
  std::function<void(AudioBackendEvent, const std::string &)> onEvent;
};

struct AudioApi
{
  std::string id;
  std::string name;
};

// the audio apis this build has, the default first
std::vector<AudioApi> audioApis();

// a backend for one of them. an empty or unknown id gives the default. null only for a
// build with no audio api at all
std::unique_ptr<AudioBackend> createAudioBackend(const std::string &apiId);

}  // namespace freeaudio::clap_wrapper::standalone
