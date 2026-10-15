#include "standalone_backends.h"
#include "standalone_details.h"

#include <algorithm>
#include <atomic>

#if WIN && CLAP_WRAPPER_HAS_WIN32
#include <Windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtAudio.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#ifdef CLAP_WRAPPER_STANDALONE_ASIO
// from the asio sdk's asio.h, which rtaudio keeps private; asio.cpp is compiled into it
long ASIOControlPanel(void);
#endif

/*
 * RtAudio behind the AudioBackend interface: every api with
 * CLAP_WRAPPER_STANDALONE_RTAUDIO, which is what Linux always builds, and ASIO next to
 * the native WASAPI on Windows built with it. A device's id is RtAudio's name for it,
 * which is also what settings files from before the native backends hold.
 */

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
#if WIN && CLAP_WRAPPER_HAS_WIN32
// PKEY_Device_FriendlyName and PKEY_AudioEndpoint_FormFactor, spelled out: the headers
// only declare them, and defining them there (INITGUID) would clash with rtaudio's
const PROPERTYKEY friendlyNameKey{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY formFactorKey{
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};

std::wstring toWide(const std::string &text)
{
  if (text.empty()) return {};
  const auto length{
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)};
  std::wstring wide(static_cast<size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

// the form factor of the active capture endpoint wasapi calls by this name, which is the
// name rtaudio gives it; false if there is none, as for every asio driver
bool captureFormFactor(const std::string &name, UINT &formFactor)
{
  const auto wanted{toWide(name)};
  if (wanted.empty()) return false;

  IMMDeviceEnumerator *enumerator{nullptr};
  if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))))
    return false;

  bool found{false};
  IMMDeviceCollection *endpoints{nullptr};
  if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &endpoints)))
  {
    UINT count{0};
    endpoints->GetCount(&count);
    for (UINT i = 0; i < count && !found; ++i)
    {
      IMMDevice *endpoint{nullptr};
      IPropertyStore *properties{nullptr};
      if (SUCCEEDED(endpoints->Item(i, &endpoint)) &&
          SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties)))
      {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(friendlyNameKey, &value)) && value.vt == VT_LPWSTR &&
            wanted == value.pwszVal)
        {
          PROPVARIANT factor;
          ::PropVariantInit(&factor);
          if (SUCCEEDED(properties->GetValue(formFactorKey, &factor)) && factor.vt == VT_UI4)
          {
            formFactor = factor.ulVal;
            found = true;
          }
          ::PropVariantClear(&factor);
        }
        ::PropVariantClear(&value);
      }
      if (properties) properties->Release();
      if (endpoint) endpoint->Release();
    }
    endpoints->Release();
  }
  enumerator->Release();

  return found;
}

// what the platform tells about the devices, which rtaudio does not: one list for each
// listing of rtaudio's
struct PlatformTraits
{
  // what windows tells about the device rtaudio calls by this name. a microphone by the
  // form factor windows gives its endpoint, which is what names it "Microphone (...)": a
  // headset's and a phone's count too. that includes inputs of simple usb interfaces that
  // report themselves as one, which a default leaves closed all the same
  void add(AudioDevice &device) const
  {
    if (device.inputChannels == 0) return;
    UINT formFactor{UnknownFormFactor};
    device.isMicrophone = captureFormFactor(device.legacyName, formFactor) &&
                          (formFactor == Microphone || formFactor == Headset || formFactor == Handset);
  }
};
#elif MAC
struct PlatformTraits
{
  // coreaudio's own listing, by rtaudio's name for each device
  std::vector<AudioDevice> core{listCoreAudioDevices()};

  void add(AudioDevice &device) const
  {
    for (const auto &c : core)
    {
      if (c.legacyName != device.legacyName) continue;
      device.isMicrophone = device.inputChannels > 0 && c.isMicrophone;
      device.isBluetooth = c.isBluetooth;
      device.isInterface = c.isInterface;
      device.minBufferFrames = c.minBufferFrames;
      device.maxBufferFrames = c.maxBufferFrames;
      device.clockDomain = c.clockDomain;
      return;
    }
  }
};
#else
struct PlatformTraits
{
  void add(AudioDevice &) const
  {
  }
};
#endif

AudioDevice toDevice(const RtAudio::DeviceInfo &info, const PlatformTraits &traits)
{
  AudioDevice device;
  device.id = info.name;
  device.name = info.name;
  device.legacyName = info.name;
  device.inputChannels = info.inputChannels;
  device.outputChannels = info.outputChannels;
  device.sampleRates.assign(info.sampleRates.begin(), info.sampleRates.end());
  std::sort(device.sampleRates.begin(), device.sampleRates.end());
  device.sampleRates.erase(std::unique(device.sampleRates.begin(), device.sampleRates.end()),
                           device.sampleRates.end());
  device.currentSampleRate =
      info.currentSampleRate > 0 ? info.currentSampleRate : info.preferredSampleRate;
  device.isDefaultInput = info.isDefaultInput;
  device.isDefaultOutput = info.isDefaultOutput;
  traits.add(device);
  return device;
}

class RtAudioBackend final : public AudioBackend
{
 public:
  explicit RtAudioBackend(RtAudio::Api requested)
  {
    dac = std::make_unique<RtAudio>(
        requested, [this](RtAudioErrorType type, const std::string &text) { onError(type, text); });
    api = dac->getCurrentApi();
    dac->showWarnings(true);
  }

  ~RtAudioBackend() override
  {
    stop();
  }

  std::string apiId() const override
  {
    return RtAudio::getApiName(api);
  }

  std::string apiDisplayName() const override
  {
    return RtAudio::getApiDisplayName(api);
  }

  // Listing is not cheap and is not side-effect free. RtAudio's ASIO backend loads,
  // ASIOInit()s and unloads *every* installed ASIO driver on each probe, and some
  // drivers do not survive that being done repeatedly - the Generic Low Latency driver
  // starts failing to allocate buffers. So the list is made once and reused until
  // refreshDevices().
  const std::vector<AudioDevice> &devices() override
  {
    if (!listValid)
    {
      listing = true;
      deviceCache.clear();
      rtIds.clear();
      const PlatformTraits traits;
      for (auto id : dac->getDeviceIds())
      {
        deviceCache.push_back(toDevice(dac->getDeviceInfo(id), traits));
        rtIds.push_back(id);
      }
      listing = false;
      listValid = true;
    }
    return deviceCache;
  }

  void refreshDevices() override
  {
    listValid = false;
  }

  std::string defaultInputDevice() override
  {
    return nameOf(dac->getDefaultInputDevice());
  }

  std::string defaultOutputDevice() override
  {
    return nameOf(dac->getDefaultOutputDevice());
  }

  bool open(const AudioStreamRequest &request, AudioStreamInfo &granted, std::string &error) override
  {
    stop();

    const bool useOutput{!request.outputDevice.empty() && request.outputChannels > 0};
    const bool useInput{!request.inputDevice.empty() && request.inputChannels > 0};

    RtAudio::StreamParameters oParams, iParams;
    if (useOutput && !rtIdOf(request.outputDevice, oParams.deviceId))
    {
      error = "The output device '" + request.outputDevice + "' is not present";
      return false;
    }
    if (useInput && !rtIdOf(request.inputDevice, iParams.deviceId))
    {
      error = "The input device '" + request.inputDevice + "' is not present";
      return false;
    }
    oParams.nChannels = request.outputChannels;
    oParams.firstChannel = request.outputFirstChannel;
    iParams.nChannels = request.inputChannels;
    iParams.firstChannel = request.inputFirstChannel;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_NONINTERLEAVED;

    // openStream writes the size it actually granted back through this
    unsigned int frames{request.bufferFrames > 0 ? request.bufferFrames : 256};

    opening = true;
    const auto result{dac->openStream(useOutput ? &oParams : nullptr, useInput ? &iParams : nullptr,
                                      RTAUDIO_FLOAT32, request.sampleRate, &frames, &rtCallback, this,
                                      &options)};
    opening = false;

    if (result != RTAUDIO_NO_ERROR || !dac->isStreamOpen())
    {
      error = dac->getErrorText();
      if (dac->isStreamOpen()) dac->closeStream();
      return false;
    }

    inputChannels = useInput ? iParams.nChannels : 0;
    outputChannels = useOutput ? oParams.nChannels : 0;
    bufferFrames = frames;

    granted = {};
    granted.sampleRate = dac->getStreamSampleRate();
    granted.bufferFrames = frames;
    granted.maxFramesPerCallback = frames;
    granted.inputChannels = inputChannels;
    granted.outputChannels = outputChannels;
    return true;
  }

  bool start(AudioStreamCallback &cb, std::string &error) override
  {
    if (!dac->isStreamOpen())
    {
      error = "No audio stream is open";
      return false;
    }

    callback = &cb;
    if (dac->startStream() != RTAUDIO_NO_ERROR)
    {
      error = dac->getErrorText();
      stop();
      return false;
    }
    return true;
  }

  void stop() override
  {
    if (dac->isStreamRunning()) dac->stopStream();
    if (dac->isStreamOpen()) dac->closeStream();
    callback = nullptr;
  }

  bool isOpen() const override
  {
    return dac->isStreamOpen();
  }

  bool isRunning() const override
  {
    return dac->isStreamRunning();
  }

  bool hasControlPanel() const override
  {
#ifdef CLAP_WRAPPER_STANDALONE_ASIO
    return api == RtAudio::Api::WINDOWS_ASIO;
#else
    return false;
#endif
  }

  bool showControlPanel() override
  {
#ifdef CLAP_WRAPPER_STANDALONE_ASIO
    // the driver is loaded only while rtaudio has a stream open on it
    if (api != RtAudio::Api::WINDOWS_ASIO || !dac->isStreamOpen()) return false;
    return ASIOControlPanel() == 0;  // ASE_OK
#else
    return false;
#endif
  }

 private:
  static int rtCallback(void *outputBuffer, void *inputBuffer, unsigned int frames, double,
                        RtAudioStreamStatus, void *data)
  {
    auto *self{static_cast<RtAudioBackend *>(data)};
    auto *callback{self->callback.load()};
    if (!callback) return 0;

    // non-interleaved: one block of frames per channel, one after the other
    float *outputs[maxChannels]{};
    const float *inputs[maxChannels]{};
    const auto outCount{std::min(self->outputChannels, maxChannels)};
    const auto inCount{std::min(self->inputChannels, maxChannels)};
    for (uint32_t ch = 0; outputBuffer && ch < outCount; ++ch)
      outputs[ch] = static_cast<float *>(outputBuffer) + ch * frames;
    for (uint32_t ch = 0; inputBuffer && ch < inCount; ++ch)
      inputs[ch] = static_cast<const float *>(inputBuffer) + ch * frames;

    callback->process(inputBuffer ? inputs : nullptr, inputBuffer ? inCount : 0,
                      outputBuffer ? outputs : nullptr, outputBuffer ? outCount : 0, frames);
    return 0;
  }

  void onError(RtAudioErrorType type, const std::string &text)
  {
    if (type == RTAUDIO_OUTPUT_UNDERFLOW || type == RTAUDIO_INPUT_OVERFLOW)
    {
      if (!reportedXrun.exchange(true))
        LOGINFO("[ERROR] RtAudio reports under/overflow '{}' [{}]", text, (int)type);
      return;
    }

    LOGINFO("[ERROR] RtAudio reports '{}' [{}]", text, (int)type);

    // errors from listing and opening reach the host through those calls' results
    if (listing || opening || type == RTAUDIO_WARNING || !onEvent) return;

    // an asio driver asked for a reset, and rtaudio closes the stream for it
    if (type == RTAUDIO_DEVICE_DISCONNECT && api == RtAudio::Api::WINDOWS_ASIO)
    {
      onEvent(AudioBackendEvent::ResetRequested, text);
      return;
    }

    if (type == RTAUDIO_DEVICE_DISCONNECT || type == RTAUDIO_SYSTEM_ERROR ||
        type == RTAUDIO_THREAD_ERROR || type == RTAUDIO_DRIVER_ERROR)
    {
      onEvent(AudioBackendEvent::StreamStopped, text);
    }
  }

  std::string nameOf(unsigned int rtId)
  {
    const auto &list{devices()};
    for (size_t i = 0; i < rtIds.size(); ++i)
    {
      if (rtIds[i] == rtId) return list[i].id;
    }
    return {};
  }

  bool rtIdOf(const std::string &id, unsigned int &rtId)
  {
    const auto &list{devices()};
    for (size_t i = 0; i < list.size(); ++i)
    {
      if (list[i].id == id)
      {
        rtId = rtIds[i];
        return true;
      }
    }
    return false;
  }

  static constexpr uint32_t maxChannels{64};

  std::unique_ptr<RtAudio> dac;
  RtAudio::Api api{RtAudio::Api::UNSPECIFIED};

  std::vector<AudioDevice> deviceCache;
  std::vector<unsigned int> rtIds;  // rtaudio's id of each in deviceCache
  bool listValid{false};

  // rtaudio reports these through the error callback too
  std::atomic<bool> listing{false}, opening{false};
  std::atomic<bool> reportedXrun{false};

  std::atomic<AudioStreamCallback *> callback{nullptr};
  uint32_t inputChannels{0}, outputChannels{0}, bufferFrames{0};
};

// the apis this build takes from rtaudio
std::vector<RtAudio::Api> usedApis()
{
  std::vector<RtAudio::Api> apis;
  RtAudio::getCompiledApi(apis);

#if !CLAP_WRAPPER_STANDALONE_RTAUDIO
  // asio only, next to the native wasapi
  apis.erase(std::remove_if(apis.begin(), apis.end(),
                            [](auto api) { return api != RtAudio::Api::WINDOWS_ASIO; }),
             apis.end());
#elif WIN
  // a first run takes wasapi, the api that is always there and always works on a stock
  // windows; rtaudio's own order puts asio first
  auto wasapi{std::find(apis.begin(), apis.end(), RtAudio::Api::WINDOWS_WASAPI)};
  if (wasapi != apis.end()) std::rotate(apis.begin(), wasapi, wasapi + 1);
#endif
  return apis;
}
}  // namespace

std::vector<AudioApi> rtAudioApis()
{
  std::vector<AudioApi> res;
  for (auto api : usedApis()) res.push_back({RtAudio::getApiName(api), RtAudio::getApiDisplayName(api)});
  return res;
}

std::unique_ptr<AudioBackend> createRtAudioBackend(const std::string &apiId)
{
  auto api{RtAudio::getCompiledApiByName(apiId)};

#if CLAP_WRAPPER_STANDALONE_RTAUDIO && !WIN
  // the default lets rtaudio pick, which takes the first api with a device
  if (apiId.empty()) api = RtAudio::Api::UNSPECIFIED;
#else
  if (api == RtAudio::Api::UNSPECIFIED)
  {
    const auto apis{usedApis()};
    if (apis.empty()) return nullptr;
    api = apis.front();
  }
#endif

  return std::make_unique<RtAudioBackend>(api);
}
}  // namespace freeaudio::clap_wrapper::standalone
