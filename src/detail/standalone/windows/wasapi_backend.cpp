/*
 * The standalone's WASAPI backend: shared mode, event driven, one thread.
 *
 * Every active render and capture endpoint is a device of its own, as RtAudio listed them
 * (renders first), and RtAudio's name for one is its friendly name. A stream asks the
 * engine for float at the rate the user chose and lets it convert (AUTOCONVERTPCM), which
 * is what RtAudio's resampler did for a rate that is not the mix rate. A buffer smaller
 * than the engine's period at the mix rate goes through IAudioClient3's small periods
 * where the driver offers them; that stream takes the mix format as it is.
 *
 * The output drives: each of its events writes whole buffers of the granted size while
 * there is room for one, so that every process() call has the same number of frames. A
 * capture endpoint beside it runs on its own clock and is read without events into a
 * ring, which process() takes its input from: when the ring has too little the input is
 * a buffer of silence (and keeps what it had, which gives the ring a cushion), and when
 * it has too much the oldest frames are dropped. With no output the capture event drives.
 *
 * Nothing in here throws.
 */

#include "detail/standalone/standalone_backends.h"

#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <devicetopology.h>
#include <mmreg.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// the keys and guids used here, spelled out: the headers only declare them, and defining
// them there (INITGUID) would clash with RtAudio's in a build that has ASIO
const PROPERTYKEY friendlyNameKey{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY formFactorKey{
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 0};
// PKEY_AudioEngine_DeviceFormat: the mix format changed in the sound control panel
const PROPERTYKEY deviceFormatKey{
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
const GUID subtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID subtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

constexpr REFERENCE_TIME hnsPerSecond{10'000'000};

// the buffer sizes offered: the engine's periods are 10 ms, a smaller block is cut from
// one and a bigger one is buffered here
constexpr uint32_t minBufferFrames{16};
constexpr uint32_t maxBufferFrames{4096};

template <typename T>
struct ComPtr
{
  T *p{nullptr};
  ComPtr() = default;
  ComPtr(const ComPtr &) = delete;
  ComPtr &operator=(const ComPtr &) = delete;
  ~ComPtr()
  {
    reset();
  }
  void reset()
  {
    if (p) p->Release();
    p = nullptr;
  }
  T **put()
  {
    reset();
    return &p;
  }
  void **putVoid()
  {
    return reinterpret_cast<void **>(put());
  }
  T *operator->() const
  {
    return p;
  }
  explicit operator bool() const
  {
    return p != nullptr;
  }
};

// CoInitializeEx for the thread it is made on, undone where it succeeded. a thread that
// already has an apartment of the other kind keeps it, and COM works there all the same
struct ComScope
{
  bool owns{false};
  explicit ComScope(DWORD model)
  {
    const auto hr{::CoInitializeEx(nullptr, model)};
    owns = SUCCEEDED(hr);
  }
  ~ComScope()
  {
    if (owns) ::CoUninitialize();
  }
};

std::string toUtf8(const wchar_t *text)
{
  if (!text || !*text) return {};
  const auto length{::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr)};
  if (length <= 1) return {};
  std::string res(static_cast<size_t>(length - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, res.data(), length, nullptr, nullptr);
  return res;
}

std::wstring toWide(const std::string &text)
{
  if (text.empty()) return {};
  const auto length{
      ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)};
  std::wstring res(static_cast<size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), res.data(), length);
  return res;
}

std::string endpointId(IMMDevice *device)
{
  LPWSTR id{nullptr};
  if (FAILED(device->GetId(&id)) || !id) return {};
  auto res{toUtf8(id)};
  ::CoTaskMemFree(id);
  return res;
}

std::string hresultText(HRESULT hr)
{
  char text[16];
  snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
  return text;
}

uint32_t framesOf(REFERENCE_TIME duration, uint32_t rate)
{
  return static_cast<uint32_t>((duration * rate + hnsPerSecond / 2) / hnsPerSecond);
}

REFERENCE_TIME durationOf(uint32_t frames, uint32_t rate)
{
  return (static_cast<REFERENCE_TIME>(frames) * hnsPerSecond + rate - 1) / rate;
}

// the sample formats a mix format can have here
enum class SampleFormat
{
  Unsupported,
  Float32,
  Int16,
  Int24,  // packed, three bytes
  Int32,  // also 24 valid bits in a 32 bit container, which are the top ones
};

SampleFormat sampleFormatOf(const WAVEFORMATEX *format)
{
  auto isFloat{format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT};
  auto isPcm{format->wFormatTag == WAVE_FORMAT_PCM};
  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22)
  {
    const auto &sub{reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format)->SubFormat};
    isFloat = IsEqualGUID(sub, subtypeFloat) != 0;
    isPcm = IsEqualGUID(sub, subtypePcm) != 0;
  }
  if (isFloat && format->wBitsPerSample == 32) return SampleFormat::Float32;
  if (isPcm && format->wBitsPerSample == 16) return SampleFormat::Int16;
  if (isPcm && format->wBitsPerSample == 24) return SampleFormat::Int24;
  if (isPcm && format->wBitsPerSample == 32) return SampleFormat::Int32;
  return SampleFormat::Unsupported;
}

// channels [first, first + count) of interleaved frames into one float buffer each
void readFrames(const BYTE *data, SampleFormat format, uint32_t frameChannels, uint32_t first,
                uint32_t count, uint32_t frames, float *const *out, uint32_t offset)
{
  for (uint32_t f = 0; f < frames; ++f)
  {
    for (uint32_t c = 0; c < count; ++c)
    {
      const auto index{static_cast<size_t>(f) * frameChannels + first + c};
      float value{0.f};
      switch (format)
      {
        case SampleFormat::Float32:
          value = reinterpret_cast<const float *>(data)[index];
          break;
        case SampleFormat::Int16:
          value = reinterpret_cast<const int16_t *>(data)[index] * (1.f / 32768.f);
          break;
        case SampleFormat::Int24:
        {
          const auto *b{data + index * 3};
          const int32_t v{static_cast<int32_t>((static_cast<uint32_t>(b[0]) << 8) |
                                               (static_cast<uint32_t>(b[1]) << 16) |
                                               (static_cast<uint32_t>(b[2]) << 24))};
          value = static_cast<float>(v) * (1.f / 2147483648.f);
          break;
        }
        case SampleFormat::Int32:
          value =
              static_cast<float>(reinterpret_cast<const int32_t *>(data)[index]) * (1.f / 2147483648.f);
          break;
        default:
          break;
      }
      out[c][offset + f] = value;
    }
  }
}

int32_t toInt32(float value)
{
  const auto v{std::clamp(static_cast<double>(value), -1.0, 1.0) * 2147483648.0};
  return static_cast<int32_t>(std::clamp(v, -2147483648.0, 2147483647.0));
}

// one float buffer each into channels [first, first + count) of interleaved frames, and
// silence into every other channel
void writeFrames(BYTE *data, SampleFormat format, uint32_t frameChannels, uint32_t first, uint32_t count,
                 uint32_t frames, const float *const *in, uint32_t bytesPerFrame)
{
  std::memset(data, 0, static_cast<size_t>(frames) * bytesPerFrame);
  for (uint32_t f = 0; f < frames; ++f)
  {
    for (uint32_t c = 0; c < count; ++c)
    {
      const auto index{static_cast<size_t>(f) * frameChannels + first + c};
      const auto value{in[c][f]};
      switch (format)
      {
        case SampleFormat::Float32:
          reinterpret_cast<float *>(data)[index] = value;
          break;
        case SampleFormat::Int16:
          reinterpret_cast<int16_t *>(data)[index] = static_cast<int16_t>(toInt32(value) >> 16);
          break;
        case SampleFormat::Int24:
        {
          const auto v{static_cast<uint32_t>(toInt32(value))};
          auto *b{data + index * 3};
          b[0] = static_cast<BYTE>(v >> 8);
          b[1] = static_cast<BYTE>(v >> 16);
          b[2] = static_cast<BYTE>(v >> 24);
          break;
        }
        case SampleFormat::Int32:
          reinterpret_cast<int32_t *>(data)[index] = toInt32(value);
          break;
        default:
          break;
      }
    }
  }
}

// the device a capture or render endpoint is on, by the kernel streaming interface the
// endpoint's connector leads to: "\\?\usb#vid_...", "\\?\bthhfenum#...". empty if unknown
std::string adapterInterfaceOf(IMMDevice *device)
{
  ComPtr<IDeviceTopology> topology;
  if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, topology.putVoid())))
    return {};
  ComPtr<IConnector> connector;
  if (FAILED(topology->GetConnector(0, connector.put()))) return {};
  LPWSTR id{nullptr};
  if (FAILED(connector->GetDeviceIdConnectedTo(&id)) || !id) return {};
  auto res{toUtf8(id)};
  ::CoTaskMemFree(id);
  std::transform(res.begin(), res.end(), res.begin(),
                 [](char c) { return static_cast<char>(::tolower(static_cast<unsigned char>(c))); });
  return res;
}

class WasapiBackend;

// IMMNotificationClient for the backend, alive for as long as COM holds it. the backend
// detaches it before it goes, so that no callback reaches a backend that is gone
class NotificationClient final : public IMMNotificationClient
{
 public:
  explicit NotificationClient(std::function<void()> changed) : changed(std::move(changed))
  {
  }

  void detach()
  {
    std::lock_guard<std::mutex> lock(mutex);
    changed = nullptr;
  }

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return ++refs;
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    const auto res{--refs};
    if (res == 0) delete this;
    return res;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override
  {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(IMMNotificationClient)))
    {
      AddRef();
      *object = static_cast<IMMNotificationClient *>(this);
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
  {
    notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
  {
    notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
  {
    notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole role, LPCWSTR) override
  {
    // once per change: windows reports each of the three roles
    if (role == eConsole) notify();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override
  {
    // windows reports many properties, often; only the format changes what we list
    if (IsEqualGUID(key.fmtid, deviceFormatKey.fmtid) && key.pid == deviceFormatKey.pid) notify();
    return S_OK;
  }

 private:
  void notify()
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (changed) changed();
  }

  std::atomic<ULONG> refs{1};
  std::mutex mutex;
  std::function<void()> changed;
};

// one side of a stream: an endpoint with its client and the format it was opened with
struct Side
{
  std::string name;
  ComPtr<IMMDevice> device;
  ComPtr<IAudioClient> client;
  ComPtr<IAudioRenderClient> render;
  ComPtr<IAudioCaptureClient> capture;
  HANDLE event{nullptr};
  SampleFormat format{SampleFormat::Unsupported};
  uint32_t frameChannels{0}, bytesPerFrame{0};
  uint32_t first{0}, count{0};
  uint32_t bufferSize{0};

  void close()
  {
    render.reset();
    capture.reset();
    client.reset();
    device.reset();
    if (event) ::CloseHandle(event);
    event = nullptr;
    count = 0;
  }
  bool isOpen() const
  {
    return static_cast<bool>(client);
  }
};

// a stream thread's failure, for StreamStopped
struct StreamFailure
{
  HRESULT hr{S_OK};
  std::string device;
};

class WasapiBackend final : public AudioBackend
{
 public:
  WasapiBackend() : com(COINIT_APARTMENTTHREADED)
  {
    ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                       enumerator.putVoid());
    stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (enumerator)
    {
      notifications = new NotificationClient(
          [this]
          {
            // listed again by refreshDevices(), on the main thread: the host holds on to
            // the devices between its calls
            if (onEvent) onEvent(AudioBackendEvent::DevicesChanged, {});
          });
      if (FAILED(enumerator->RegisterEndpointNotificationCallback(notifications)))
      {
        notifications->Release();
        notifications = nullptr;
      }
    }
  }

  ~WasapiBackend() override
  {
    stop();
    if (notifications)
    {
      enumerator->UnregisterEndpointNotificationCallback(notifications);
      notifications->detach();
      notifications->Release();
    }
    enumerator.reset();
    if (stopEvent) ::CloseHandle(stopEvent);
  }

  std::string apiId() const override
  {
    return "wasapi";
  }
  std::string apiDisplayName() const override
  {
    return "WASAPI";
  }

  const std::vector<AudioDevice> &devices() override
  {
    if (!listed.exchange(true)) list();
    return deviceList;
  }

  void refreshDevices() override
  {
    listed = false;
  }

  std::string defaultInputDevice() override
  {
    return defaultEndpoint(eCapture);
  }
  std::string defaultOutputDevice() override
  {
    return defaultEndpoint(eRender);
  }

  bool open(const AudioStreamRequest &request, AudioStreamInfo &granted, std::string &error) override
  {
    stop();
    granted = {};

    if (!enumerator)
    {
      error = "Windows audio (WASAPI) is not available.";
      return false;
    }

    const bool hasOutput{!request.outputDevice.empty() && request.outputChannels > 0};
    const bool hasInput{!request.inputDevice.empty() && request.inputChannels > 0};
    if (!hasOutput && !hasInput)
    {
      error = "No audio device was chosen.";
      return false;
    }

    // the rate: the request's, or the output's mix rate, else the input's
    uint32_t rate{request.sampleRate};
    if (!acquire(output, hasOutput ? request.outputDevice : std::string{}, error) ||
        !acquire(input, hasInput ? request.inputDevice : std::string{}, error))
    {
      closeSides();
      return false;
    }
    const auto &driving{hasOutput ? output : input};
    WAVEFORMATEX *mix{nullptr};
    if (FAILED(driving.client->GetMixFormat(&mix)))
    {
      error = "The audio device " + driving.name + " does not report its format.";
      closeSides();
      return false;
    }
    const uint32_t mixRate{mix->nSamplesPerSec};
    REFERENCE_TIME defaultPeriod{0}, minimumPeriod{0};
    driving.client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);
    ::CoTaskMemFree(mix);
    if (rate == 0) rate = mixRate;

    const uint32_t enginePeriod{std::max<uint32_t>(framesOf(defaultPeriod, rate), 1)};
    const uint32_t frames{request.bufferFrames ? std::min(request.bufferFrames, maxBufferFrames)
                                               : enginePeriod};

    // the output, or the input that drives in its place, gets the event
    uint32_t period{0};
    bool ok{true};
    if (hasOutput)
      ok = initialize(output, request.outputFirstChannel, request.outputChannels, rate, frames, true,
                      period, error);
    if (ok && hasInput)
    {
      uint32_t inputPeriod{0};
      ok = initialize(input, request.inputFirstChannel, request.inputChannels, rate, frames, !hasOutput,
                      inputPeriod, error);
      if (!hasOutput) period = inputPeriod;
    }
    if (!ok)
    {
      closeSides();
      return false;
    }

    bufferFrames = period ? period : frames;
    streamRate = rate;

    if (hasOutput)
      output.client->GetService(__uuidof(IAudioRenderClient),
                                reinterpret_cast<void **>(output.render.put()));
    if (hasInput)
      input.client->GetService(__uuidof(IAudioCaptureClient),
                               reinterpret_cast<void **>(input.capture.put()));
    if ((hasOutput && !output.render) || (hasInput && !input.capture))
    {
      error = "The audio stream could not be opened.";
      closeSides();
      return false;
    }

    // planar buffers for process(), and the input's ring: room for a capture buffer
    // beside the buffer process() is about to take
    inputBuffers.assign(input.count, std::vector<float>(bufferFrames, 0.f));
    outputBuffers.assign(output.count, std::vector<float>(bufferFrames, 0.f));
    ringSize = hasInput ? input.bufferSize + 4 * bufferFrames : 0;
    ring.assign(input.count, std::vector<float>(ringSize, 0.f));
    ringRead = ringFill = 0;

    // and the pointers to them, so that the audio thread allocates nothing
    inputPointers.clear();
    for (const auto &buffer : inputBuffers) inputPointers.push_back(buffer.data());
    outputPointers.clear();
    outputSources.clear();
    for (auto &buffer : outputBuffers)
    {
      outputPointers.push_back(buffer.data());
      outputSources.push_back(buffer.data());
    }
    ringTargets.clear();
    for (auto &channel : ring) ringTargets.push_back(channel.data());

    granted.sampleRate = rate;
    granted.bufferFrames = bufferFrames;
    granted.maxFramesPerCallback = bufferFrames;
    granted.inputChannels = input.count;
    granted.outputChannels = output.count;
    // between the device and the plugin's block, the block itself not included (the
    // standalone features' audio status): the engine's own latency, plus on the way out
    // what the buffer holds ahead of a block - it is kept filled to within one block of
    // full - and on the way in the capture's period, which a block beside an output
    // waits for at most, since the input is read once per output event
    granted.outputLatency =
        hasOutput ? latencyOf(output) +
                        (output.bufferSize > bufferFrames ? output.bufferSize - bufferFrames : 0)
                  : 0;
    granted.inputLatency = hasInput ? latencyOf(input) + (hasOutput ? capturePeriod(rate) : 0) : 0;

    opened = true;
    return true;
  }

  bool start(AudioStreamCallback &streamCallback, std::string &error) override
  {
    if (!opened)
    {
      error = "No audio stream is open.";
      return false;
    }
    if (running) return true;

    callback = &streamCallback;
    ::ResetEvent(stopEvent);

    // the output starts out with a buffer of silence, so that its first event finds room
    // for no more than the engine has played
    if (output.isOpen())
    {
      BYTE *data{nullptr};
      if (SUCCEEDED(output.render->GetBuffer(output.bufferSize, &data)))
        output.render->ReleaseBuffer(output.bufferSize, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    HRESULT hr{S_OK};
    if (input.isOpen()) hr = input.client->Start();
    if (SUCCEEDED(hr) && output.isOpen()) hr = output.client->Start();
    if (FAILED(hr))
    {
      error = "The audio stream could not be started (" + hresultText(hr) + ").";
      stop();
      return false;
    }

    running = true;
    thread = std::thread([this] { run(); });
    return true;
  }

  void stop() override
  {
    if (thread.joinable())
    {
      ::SetEvent(stopEvent);
      thread.join();
    }
    running = false;
    if (output.isOpen()) output.client->Stop();
    if (input.isOpen()) input.client->Stop();
    closeSides();
    callback = nullptr;
    opened = false;
  }

  bool isOpen() const override
  {
    return opened;
  }
  bool isRunning() const override
  {
    return running;
  }

 private:
  void list()
  {
    deviceList.clear();
    if (!enumerator) return;

    const auto defaultOutput{defaultEndpoint(eRender)};
    const auto defaultInput{defaultEndpoint(eCapture)};

    // renders first, as RtAudio listed them
    for (auto flow : {eRender, eCapture})
    {
      ComPtr<IMMDeviceCollection> endpoints;
      if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, endpoints.put()))) continue;
      UINT count{0};
      endpoints->GetCount(&count);
      for (UINT i = 0; i < count; ++i)
      {
        ComPtr<IMMDevice> endpoint;
        if (FAILED(endpoints->Item(i, endpoint.put()))) continue;
        AudioDevice device;
        if (describe(endpoint.p, flow == eCapture, device))
        {
          device.isDefaultOutput = flow == eRender && device.id == defaultOutput;
          device.isDefaultInput = flow == eCapture && device.id == defaultInput;
          deviceList.push_back(std::move(device));
        }
      }
    }
  }

  // a device as RtAudio's probe saw it: its name, and its channels and rate from its mix
  // format; false where that probe failed and RtAudio left the endpoint out
  bool describe(IMMDevice *endpoint, bool capture, AudioDevice &device)
  {
    device.id = endpointId(endpoint);
    if (device.id.empty()) return false;

    ComPtr<IPropertyStore> properties;
    if (FAILED(endpoint->OpenPropertyStore(STGM_READ, properties.put()))) return false;
    PROPVARIANT value;
    ::PropVariantInit(&value);
    if (SUCCEEDED(properties->GetValue(friendlyNameKey, &value)) && value.vt == VT_LPWSTR)
      device.name = toUtf8(value.pwszVal);
    ::PropVariantClear(&value);
    if (device.name.empty()) return false;
    device.legacyName = device.name;

    if (capture)
    {
      // a microphone by the form factor windows gives its endpoint, which is what names
      // it "Microphone (...)": a headset's and a phone's count too. that includes inputs
      // of simple usb interfaces that report themselves as one, which a default leaves
      // closed all the same
      ::PropVariantInit(&value);
      if (SUCCEEDED(properties->GetValue(formFactorKey, &value)) && value.vt == VT_UI4)
        device.isMicrophone =
            value.ulVal == Microphone || value.ulVal == Headset || value.ulVal == Handset;
      ::PropVariantClear(&value);
    }

    const auto adapter{adapterInterfaceOf(endpoint)};
    device.isBluetooth = adapter.find("bthenum") != std::string::npos ||
                         adapter.find("bthhfenum") != std::string::npos ||
                         adapter.find("bthle") != std::string::npos;

    ComPtr<IAudioClient> client;
    if (FAILED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid())))
      return false;
    WAVEFORMATEX *mix{nullptr};
    if (FAILED(client->GetMixFormat(&mix))) return false;
    (capture ? device.inputChannels : device.outputChannels) = mix->nChannels;
    device.currentSampleRate = mix->nSamplesPerSec;

    // every standard rate up to 192 kHz, through the engine's converter, as RtAudio
    // offered them through its own, and the mix rate
    for (auto rate : standardSampleRates())
    {
      if (rate <= 192000) device.sampleRates.push_back(rate);
    }
    device.sampleRates.push_back(mix->nSamplesPerSec);
    std::sort(device.sampleRates.begin(), device.sampleRates.end());
    device.sampleRates.erase(std::unique(device.sampleRates.begin(), device.sampleRates.end()),
                             device.sampleRates.end());
    device.sampleRateSettable = true;

    // any size runs: one smaller than the engine's period is cut from it, one larger is
    // buffered here, up to maxBufferFrames. a smaller block still shortens the way out a
    // little (the buffer is kept filled to within one block), so the sizes below the
    // period are offered too
    device.minBufferFrames = minBufferFrames;
    device.maxBufferFrames = std::max(device.minBufferFrames, maxBufferFrames);
    ::CoTaskMemFree(mix);
    return true;
  }

  std::string defaultEndpoint(EDataFlow flow)
  {
    if (!enumerator) return {};
    ComPtr<IMMDevice> endpoint;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, endpoint.put()))) return {};
    return endpointId(endpoint.p);
  }

  // the endpoint and its client for one side; true with nothing done for an empty id
  bool acquire(Side &side, const std::string &id, std::string &error)
  {
    if (id.empty()) return true;
    const auto *known{findDevice(devices(), id, &side == &input)};
    side.name = known ? known->name : id;
    const auto wide{toWide(known ? known->id : id)};
    if (FAILED(enumerator->GetDevice(wide.c_str(), side.device.put())))
    {
      error = "The audio device " + side.name + " is not there.";
      return false;
    }
    DWORD state{0};
    if (FAILED(side.device->GetState(&state)) || state != DEVICE_STATE_ACTIVE)
    {
      error = "The audio device " + side.name + " is not connected or is disabled.";
      return false;
    }
    if (FAILED(
            side.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, side.client.putVoid())))
    {
      error = "The audio device " + side.name + " could not be opened.";
      return false;
    }
    return true;
  }

  // initializes a side's client: IAudioClient3's small period for a buffer below the
  // engine's at the mix rate where the driver has one, else the classic shared stream,
  // in float at the stream's rate, converted by the engine. withEvent for the side that
  // drives. period: the engine's period in frames for that side
  bool initialize(Side &side, uint32_t first, uint32_t count, uint32_t rate, uint32_t frames,
                  bool withEvent, uint32_t &period, std::string &error)
  {
    WAVEFORMATEX *mix{nullptr};
    if (FAILED(side.client->GetMixFormat(&mix)))
    {
      error = "The audio device " + side.name + " does not report its format.";
      return false;
    }
    if (first + count > mix->nChannels)
    {
      error = "The audio device " + side.name + " has " + std::to_string(mix->nChannels) +
              " channels, not " + std::to_string(first + count) + ".";
      ::CoTaskMemFree(mix);
      return false;
    }

    const DWORD eventFlag{withEvent ? static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK) : 0};
    REFERENCE_TIME defaultPeriod{0}, minimumPeriod{0};
    side.client->GetDevicePeriod(&defaultPeriod, &minimumPeriod);

    HRESULT hr{E_FAIL};
    bool smallPeriod{false};

    // a small period: the mix format as it is, at the mix rate
    if (withEvent && rate == mix->nSamplesPerSec && frames < framesOf(defaultPeriod, rate))
    {
      ComPtr<IAudioClient3> client3;
      if (SUCCEEDED(
              side.device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, client3.putVoid())))
      {
        UINT32 standard{0}, fundamental{0}, minimum{0}, maximum{0};
        if (SUCCEEDED(
                client3->GetSharedModeEnginePeriod(mix, &standard, &fundamental, &minimum, &maximum)) &&
            fundamental > 0 && minimum < standard)
        {
          auto wanted{std::clamp<UINT32>(frames, minimum, maximum)};
          wanted = (wanted + fundamental - 1) / fundamental * fundamental;
          wanted = std::clamp<UINT32>(wanted, minimum, maximum);
          hr = client3->InitializeSharedAudioStream(eventFlag, wanted, mix, nullptr);
          if (SUCCEEDED(hr))
          {
            side.client.reset();
            side.client.p = client3.p;
            client3.p = nullptr;
            smallPeriod = true;
            period = wanted;
            side.format = sampleFormatOf(mix);
            side.frameChannels = mix->nChannels;
            side.bytesPerFrame = mix->nBlockAlign;
          }
          else
          {
            // a failed Initialize leaves the client unusable: start again with a new one
            side.client.reset();
            side.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, side.client.putVoid());
            if (!side.client)
            {
              error = "The audio device " + side.name + " could not be opened.";
              ::CoTaskMemFree(mix);
              return false;
            }
          }
        }
      }
    }

    if (!smallPeriod)
    {
      // float in the mix format's channel layout, at our rate; the engine converts
      WAVEFORMATEXTENSIBLE format{};
      format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      format.Format.nChannels = mix->nChannels;
      format.Format.nSamplesPerSec = rate;
      format.Format.wBitsPerSample = 32;
      format.Format.nBlockAlign = static_cast<WORD>(mix->nChannels * 4);
      format.Format.nAvgBytesPerSec = rate * format.Format.nBlockAlign;
      format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      format.Samples.wValidBitsPerSample = 32;
      format.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22
                                 ? reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix)->dwChannelMask
                                 : 0;
      format.SubFormat = subtypeFloat;

      // room for the engine's period and one buffer of ours beside it, twice that for a
      // capture we read without events; at least the engine's own minimum
      const auto periodFrames{framesOf(defaultPeriod, rate)};
      const auto wanted{withEvent ? periodFrames + 2 * frames : 2 * (periodFrames + frames)};
      const auto duration{durationOf(wanted, rate)};

      hr = side.client->Initialize(
          AUDCLNT_SHAREMODE_SHARED,
          eventFlag | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
          duration, 0, &format.Format, nullptr);
      if (FAILED(hr))
      {
        error = "The audio device " + side.name + " could not be opened at " + std::to_string(rate) +
                " Hz (" + hresultText(hr) + ").";
        ::CoTaskMemFree(mix);
        return false;
      }
      period = frames;
      side.format = SampleFormat::Float32;
      side.frameChannels = mix->nChannels;
      side.bytesPerFrame = format.Format.nBlockAlign;
    }
    ::CoTaskMemFree(mix);

    if (side.format == SampleFormat::Unsupported)
    {
      error = "The audio device " + side.name + " has a sample format this application cannot use.";
      return false;
    }

    UINT32 bufferSize{0};
    side.client->GetBufferSize(&bufferSize);
    side.bufferSize = bufferSize;
    side.first = first;
    side.count = count;

    if (withEvent)
    {
      side.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (!side.event || FAILED(side.client->SetEventHandle(side.event)))
      {
        error = "The audio device " + side.name + " could not be opened.";
        return false;
      }
    }
    return true;
  }

  // the capture engine's period in frames at the stream's rate
  uint32_t capturePeriod(uint32_t rate) const
  {
    REFERENCE_TIME period{0}, minimum{0};
    if (FAILED(input.client->GetDevicePeriod(&period, &minimum))) return 0;
    return framesOf(period, rate);
  }

  uint32_t latencyOf(Side &side) const
  {
    REFERENCE_TIME latency{0};
    if (FAILED(side.client->GetStreamLatency(&latency))) return 0;
    return framesOf(latency, streamRate);
  }

  void closeSides()
  {
    output.close();
    input.close();
    inputBuffers.clear();
    outputBuffers.clear();
    ring.clear();
  }

  // [audio-thread]
  void run()
  {
    ComScope threadCom(COINIT_MULTITHREADED);
    DWORD taskIndex{0};
    auto task{::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex)};

    StreamFailure failure;
    const HANDLE events[2]{stopEvent, output.isOpen() ? output.event : input.event};
    while (true)
    {
      const auto wait{::WaitForMultipleObjects(2, events, FALSE, 2000)};
      if (wait == WAIT_OBJECT_0) break;
      if (wait == WAIT_TIMEOUT)
      {
        failure = {AUDCLNT_E_DEVICE_INVALIDATED, output.isOpen() ? output.name : input.name};
        break;
      }
      if (wait != WAIT_OBJECT_0 + 1)
      {
        failure = {HRESULT_FROM_WIN32(::GetLastError()), output.isOpen() ? output.name : input.name};
        break;
      }
      if (!cycle(failure)) break;
    }

    if (task) ::AvRevertMmThreadCharacteristics(task);

    if (failure.hr != S_OK)
    {
      running = false;
      if (onEvent) onEvent(AudioBackendEvent::StreamStopped, messageFor(failure));
    }
  }

  static std::string messageFor(const StreamFailure &failure)
  {
    switch (failure.hr)
    {
      case AUDCLNT_E_DEVICE_INVALIDATED:
        return "The audio device " + failure.device +
               " was disconnected, disabled, stopped responding, or its format was changed.";
      case AUDCLNT_E_SERVICE_NOT_RUNNING:
        return "The Windows audio service stopped.";
      case AUDCLNT_E_RESOURCES_INVALIDATED:
        return "The audio device " + failure.device + " was reset.";
      default:
        return "The audio stream on " + failure.device + " stopped (" + hresultText(failure.hr) + ").";
    }
  }

  // [audio-thread] one event: what has been captured into the ring, then as many buffers
  // as there is room for (with an output) or as the ring holds (without one)
  bool cycle(StreamFailure &failure)
  {
    if (input.isOpen())
    {
      const auto hr{readCapture()};
      if (FAILED(hr))
      {
        failure = {hr, input.name};
        return false;
      }
    }

    if (!output.isOpen())
    {
      while (ringFill >= bufferFrames) processBuffer(bufferFrames);
      return true;
    }

    UINT32 padding{0};
    auto hr{output.client->GetCurrentPadding(&padding)};
    if (FAILED(hr))
    {
      failure = {hr, output.name};
      return false;
    }
    auto room{output.bufferSize - padding};
    const auto chunk{std::min(bufferFrames, output.bufferSize)};
    while (room >= chunk)
    {
      if (input.isOpen()) balanceRing();
      processBuffer(chunk);

      BYTE *data{nullptr};
      hr = output.render->GetBuffer(chunk, &data);
      if (FAILED(hr))
      {
        failure = {hr, output.name};
        return false;
      }
      writeFrames(data, output.format, output.frameChannels, output.first, output.count, chunk,
                  outputSources.data(), output.bytesPerFrame);
      hr = output.render->ReleaseBuffer(chunk, 0);
      if (FAILED(hr))
      {
        failure = {hr, output.name};
        return false;
      }
      room -= chunk;
    }
    return true;
  }

  // [audio-thread] the capture's packets into the ring; what does not fit is dropped
  HRESULT readCapture()
  {
    while (true)
    {
      UINT32 packet{0};
      auto hr{input.capture->GetNextPacketSize(&packet)};
      if (FAILED(hr)) return hr;
      if (packet == 0) return S_OK;

      BYTE *data{nullptr};
      UINT32 frames{0};
      DWORD flags{0};
      hr = input.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
      if (FAILED(hr)) return hr;

      const auto take{std::min(frames, ringSize - ringFill)};
      uint32_t done{0};
      while (done < take)
      {
        const auto at{(ringRead + ringFill) % ringSize};
        const auto n{std::min(take - done, ringSize - at)};
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
        {
          for (auto &channel : ring) std::fill(channel.begin() + at, channel.begin() + at + n, 0.f);
        }
        else
        {
          readFrames(data + static_cast<size_t>(done) * input.bytesPerFrame, input.format,
                     input.frameChannels, input.first, input.count, n, ringTargets.data(), at);
        }
        ringFill += n;
        done += n;
      }

      hr = input.capture->ReleaseBuffer(frames);
      if (FAILED(hr)) return hr;
    }
  }

  // [audio-thread] input and output run on clocks of their own. with too little in the
  // ring for the next buffer, that buffer's input is silence and the ring keeps what it
  // has, which leaves it a cushion from then on; with more than a capture buffer and two
  // of ours, the oldest frames go
  void balanceRing()
  {
    const auto high{input.bufferSize / 2 + 2 * bufferFrames};
    if (ringFill > high)
    {
      const auto drop{ringFill - (high - bufferFrames)};
      ringRead = (ringRead + drop) % ringSize;
      ringFill -= drop;
    }
  }

  // [audio-thread] one process() call of frames: its input from the ring, or silence
  void processBuffer(uint32_t frames)
  {
    for (uint32_t c = 0; c < input.count; ++c)
    {
      auto &buffer{inputBuffers[c]};
      if (ringFill >= frames)
      {
        for (uint32_t f = 0; f < frames; ++f) buffer[f] = ring[c][(ringRead + f) % ringSize];
      }
      else
      {
        std::fill(buffer.begin(), buffer.begin() + frames, 0.f);
      }
    }
    if (input.count && ringFill >= frames)
    {
      ringRead = (ringRead + frames) % ringSize;
      ringFill -= frames;
    }

    if (callback)
      callback->process(input.count ? inputPointers.data() : nullptr, input.count,
                        output.count ? outputPointers.data() : nullptr, output.count, frames);
  }

  ComScope com;
  ComPtr<IMMDeviceEnumerator> enumerator;
  NotificationClient *notifications{nullptr};

  std::atomic<bool> listed{false};
  std::vector<AudioDevice> deviceList;

  Side output, input;
  uint32_t streamRate{0}, bufferFrames{0};
  std::vector<std::vector<float>> inputBuffers, outputBuffers;
  std::vector<std::vector<float>> ring;
  std::vector<const float *> inputPointers, outputSources;
  std::vector<float *> outputPointers, ringTargets;
  uint32_t ringSize{0}, ringRead{0}, ringFill{0};

  AudioStreamCallback *callback{nullptr};
  HANDLE stopEvent{nullptr};
  std::thread thread;
  bool opened{false};
  std::atomic<bool> running{false};
};
}  // namespace

std::unique_ptr<AudioBackend> createWasapiBackend()
{
  return std::make_unique<WasapiBackend>();
}
}  // namespace freeaudio::clap_wrapper::standalone
