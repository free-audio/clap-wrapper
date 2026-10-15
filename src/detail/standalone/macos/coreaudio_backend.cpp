#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Block.h>
#include <dispatch/dispatch.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "detail/standalone/standalone_backends.h"
#include "detail/standalone/standalone_details.h"

/*
 * CoreAudio, for the standalone on macOS: the device list with its change notifications,
 * and a stream as an IOProc on one device, or on a private aggregate device when input and
 * output are two devices. Plain C++ with blocks, so that it builds the same with ARC and
 * without.
 */

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// kAudioObjectPropertyElementMain, which was kAudioObjectPropertyElementMaster before
// macOS 12; both are 0
constexpr AudioObjectPropertyElement elementMain{0};

// the uid of every aggregate device this standalone makes starts so, which is how its own
// process leaves them out of the list it shows
constexpr const char *aggregateUidPrefix{"org.free-audio.clap-wrapper.standalone.aggregate."};

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                   AudioObjectPropertyElement element = elementMain)
{
  return {selector, scope, element};
}

template <typename T>
bool getProperty(AudioObjectID id, const AudioObjectPropertyAddress &where, T &value)
{
  UInt32 size = sizeof(T);
  return AudioObjectHasProperty(id, &where) &&
         AudioObjectGetPropertyData(id, &where, 0, nullptr, &size, &value) == noErr;
}

template <typename T>
bool setProperty(AudioObjectID id, const AudioObjectPropertyAddress &where, const T &value)
{
  return AudioObjectSetPropertyData(id, &where, 0, nullptr, sizeof(T), &value) == noErr;
}

// a property that is an array, or a variable-length struct, as bytes
std::vector<uint8_t> getBytes(AudioObjectID id, const AudioObjectPropertyAddress &where)
{
  UInt32 size = 0;
  if (!AudioObjectHasProperty(id, &where) ||
      AudioObjectGetPropertyDataSize(id, &where, 0, nullptr, &size) != noErr || size == 0)
    return {};
  std::vector<uint8_t> bytes(size);
  if (AudioObjectGetPropertyData(id, &where, 0, nullptr, &size, bytes.data()) != noErr) return {};
  bytes.resize(size);
  return bytes;
}

template <typename T>
std::vector<T> getArray(AudioObjectID id, const AudioObjectPropertyAddress &where)
{
  const auto bytes{getBytes(id, where)};
  std::vector<T> res(bytes.size() / sizeof(T));
  if (!res.empty()) memcpy(res.data(), bytes.data(), res.size() * sizeof(T));
  return res;
}

std::string toString(CFStringRef text, CFStringEncoding encoding)
{
  if (!text) return {};
  std::vector<char> buffer(static_cast<size_t>(CFStringGetLength(text)) * 3 + 1, 0);
  if (!CFStringGetCString(text, buffer.data(), static_cast<CFIndex>(buffer.size()), encoding)) return {};
  return buffer.data();
}

// a string property, which the caller of AudioObjectGetPropertyData owns
std::string stringProperty(AudioObjectID id, const AudioObjectPropertyAddress &where,
                           CFStringEncoding encoding = kCFStringEncodingUTF8)
{
  CFStringRef text{nullptr};
  if (!getProperty(id, where, text) || !text) return {};
  auto res{toString(text, encoding)};
  CFRelease(text);
  return res;
}

AudioObjectPropertyScope scopeOf(bool input)
{
  return input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
}

// the channels of each stream of one direction, in stream order
std::vector<uint32_t> streamChannels(AudioObjectID id, bool input)
{
  const auto bytes{getBytes(id, address(kAudioDevicePropertyStreamConfiguration, scopeOf(input)))};
  if (bytes.size() < sizeof(AudioBufferList)) return {};
  const auto *buffers{reinterpret_cast<const AudioBufferList *>(bytes.data())};

  std::vector<uint32_t> res;
  for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
    res.push_back(buffers->mBuffers[i].mNumberChannels);
  return res;
}

uint32_t channelCount(AudioObjectID id, bool input)
{
  uint32_t total{0};
  for (auto channels : streamChannels(id, input)) total += channels;
  return total;
}

std::vector<AudioDeviceID> allDevices()
{
  return getArray<AudioDeviceID>(kAudioObjectSystemObject, address(kAudioHardwarePropertyDevices));
}

AudioDeviceID defaultDevice(bool input)
{
  AudioDeviceID device{kAudioObjectUnknown};
  getProperty(kAudioObjectSystemObject,
              address(input ? kAudioHardwarePropertyDefaultInputDevice
                            : kAudioHardwarePropertyDefaultOutputDevice),
              device);
  return device;
}

std::string uidOf(AudioDeviceID device)
{
  return stringProperty(device, address(kAudioDevicePropertyDeviceUID));
}

bool isBluetooth(UInt32 transport)
{
  return transport == kAudioDeviceTransportTypeBluetooth ||
         transport == kAudioDeviceTransportTypeBluetoothLE;
}

// a microphone the user is unlikely to mean when an interface is plugged in: the
// built-in one, a bluetooth headset, or an iphone's (continuity, wired and wireless)
bool isMicrophoneTransport(UInt32 transport)
{
  return transport == kAudioDeviceTransportTypeBuiltIn || isBluetooth(transport) ||
         transport == 'ccwd' || transport == 'ccwl';
}

// hardware an instrument or a line is plugged into. not virtual or aggregate devices,
// which route other applications' audio or combine devices the user built themselves
bool isInterfaceTransport(UInt32 transport)
{
  return transport == kAudioDeviceTransportTypeUSB ||
         transport == kAudioDeviceTransportTypeThunderbolt ||
         transport == kAudioDeviceTransportTypeFireWire || transport == kAudioDeviceTransportTypePCI ||
         transport == kAudioDeviceTransportTypeAVB;
}

bool isOwnAggregate(const std::string &uid)
{
  return uid.rfind(aggregateUidPrefix, 0) == 0;
}

std::vector<std::string> channelNames(AudioDeviceID device, bool input, uint32_t channels)
{
  std::vector<std::string> names;
  bool any{false};
  for (uint32_t ch = 0; ch < channels; ++ch)
  {
    names.push_back(
        stringProperty(device, address(kAudioObjectPropertyElementName, scopeOf(input), ch + 1)));
    any = any || !names.back().empty();
  }
  if (!any) names.clear();
  return names;
}

// a device as the list shows it; false for one that is not listed: dead, hidden, or ours
bool describe(AudioDeviceID id, AudioDeviceID defaultInput, AudioDeviceID defaultOutput,
              AudioDevice &device)
{
  UInt32 alive{1}, hidden{0};
  getProperty(id, address(kAudioDevicePropertyDeviceIsAlive), alive);
  getProperty(id, address(kAudioDevicePropertyIsHidden), hidden);
  if (!alive || hidden) return false;

  device = {};
  device.id = uidOf(id);
  if (device.id.empty() || isOwnAggregate(device.id)) return false;

  device.name = stringProperty(id, address(kAudioObjectPropertyName));

  // rtaudio's name: "manufacturer: name", in the system encoding
  const auto encoding{CFStringGetSystemEncoding()};
  device.legacyName = stringProperty(id, address(kAudioObjectPropertyManufacturer), encoding) + ": " +
                      stringProperty(id, address(kAudioObjectPropertyName), encoding);

  device.inputChannels = channelCount(id, true);
  device.outputChannels = channelCount(id, false);
  if (device.inputChannels == 0 && device.outputChannels == 0) return false;
  device.inputChannelNames = channelNames(id, true, device.inputChannels);
  device.outputChannelNames = channelNames(id, false, device.outputChannels);

  std::vector<std::pair<double, double>> ranges;
  for (const auto &range :
       getArray<AudioValueRange>(id, address(kAudioDevicePropertyAvailableNominalSampleRates)))
    ranges.emplace_back(range.mMinimum, range.mMaximum);
  device.sampleRates = sampleRatesFrom(ranges);

  Float64 rate{0};
  if (getProperty(id, address(kAudioDevicePropertyNominalSampleRate), rate) && rate > 0)
    device.currentSampleRate = static_cast<uint32_t>(rate + 0.5);

  UInt32 transport{0};
  getProperty(id, address(kAudioDevicePropertyTransportType), transport);
  device.isBluetooth = isBluetooth(transport);
  device.isMicrophone = device.inputChannels > 0 && isMicrophoneTransport(transport);
  device.isInterface = isInterfaceTransport(transport);

  // a bluetooth device's rate is set by its profile, and setting it fails (#53)
  Boolean settable{false};
  const auto rateAddress{address(kAudioDevicePropertyNominalSampleRate)};
  AudioObjectIsPropertySettable(id, &rateAddress, &settable);
  device.sampleRateSettable = settable && !device.isBluetooth && device.sampleRates.size() > 1;

  AudioValueRange frames{};
  if (getProperty(id, address(kAudioDevicePropertyBufferFrameSizeRange), frames) &&
      frames.mMinimum >= 0 && frames.mMaximum >= frames.mMinimum)
  {
    device.minBufferFrames = static_cast<uint32_t>(frames.mMinimum);
    device.maxBufferFrames = static_cast<uint32_t>(frames.mMaximum);
  }

  UInt32 domain{0};
  getProperty(id, address(kAudioDevicePropertyClockDomain), domain);
  device.clockDomain = domain;

  device.isDefaultInput = id == defaultInput && device.inputChannels > 0;
  device.isDefaultOutput = id == defaultOutput && device.outputChannels > 0;
  return true;
}

AudioDeviceID deviceWithUid(const std::string &uid)
{
  if (uid.empty()) return kAudioObjectUnknown;
  for (auto id : allDevices())
  {
    if (uidOf(id) == uid) return id;
  }
  return kAudioObjectUnknown;
}

std::string nameOf(AudioDeviceID id)
{
  auto name{stringProperty(id, address(kAudioObjectPropertyName))};
  return name.empty() ? std::string{"The audio device"} : "“" + name + "”";
}

// frames from the device's converters to its buffer, one way: device latency, safety
// offset and the first stream's latency
uint32_t latencyOf(AudioDeviceID id, bool input)
{
  UInt32 latency{0}, safety{0}, streamLatency{0};
  getProperty(id, address(kAudioDevicePropertyLatency, scopeOf(input)), latency);
  getProperty(id, address(kAudioDevicePropertySafetyOffset, scopeOf(input)), safety);
  const auto streams{getArray<AudioStreamID>(id, address(kAudioDevicePropertyStreams, scopeOf(input)))};
  if (!streams.empty())
    getProperty(streams.front(), address(kAudioStreamPropertyLatency), streamLatency);
  return latency + safety + streamLatency;
}

// whether every stream of one direction delivers 32-bit float, which the IOProc takes as
// it is
bool streamsAreFloat(AudioObjectID id, bool input)
{
  for (auto stream : getArray<AudioStreamID>(id, address(kAudioDevicePropertyStreams, scopeOf(input))))
  {
    AudioStreamBasicDescription format{};
    if (!getProperty(stream, address(kAudioStreamPropertyVirtualFormat), format)) return false;
    if (format.mFormatID != kAudioFormatLinearPCM || !(format.mFormatFlags & kAudioFormatFlagIsFloat) ||
        format.mBitsPerChannel != 32)
      return false;
  }
  return true;
}

CFStringRef cfString(const std::string &text)
{
  return CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
}

void setNumber(CFMutableDictionaryRef dictionary, const char *key, int value)
{
  auto cfKey{cfString(key)};
  auto number{CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value)};
  CFDictionarySetValue(dictionary, cfKey, number);
  CFRelease(number);
  CFRelease(cfKey);
}

void setString(CFMutableDictionaryRef dictionary, const char *key, const std::string &value)
{
  auto cfKey{cfString(key)};
  auto cfValue{cfString(value)};
  CFDictionarySetValue(dictionary, cfKey, cfValue);
  CFRelease(cfValue);
  CFRelease(cfKey);
}

std::string newUuid()
{
  auto uuid{CFUUIDCreate(kCFAllocatorDefault)};
  auto text{CFUUIDCreateString(kCFAllocatorDefault, uuid)};
  auto res{toString(text, kCFStringEncodingUTF8)};
  CFRelease(text);
  CFRelease(uuid);
  return res;
}

// a listener block registered on an object, removed with the same block
struct Listener
{
  AudioObjectID object{kAudioObjectUnknown};
  AudioObjectPropertyAddress where{};
  AudioObjectPropertyListenerBlock block{nullptr};
};

class CoreAudioBackend final : public AudioBackend
{
 public:
  CoreAudioBackend()
  {
    queue = dispatch_queue_create("org.free-audio.clap-wrapper.coreaudio", DISPATCH_QUEUE_SERIAL);

    // devices coming and going, and the defaults changing
    for (auto selector : {kAudioHardwarePropertyDevices, kAudioHardwarePropertyDefaultInputDevice,
                          kAudioHardwarePropertyDefaultOutputDevice})
    {
      listen(systemListeners, kAudioObjectSystemObject, address(selector),
             ^(UInt32, const AudioObjectPropertyAddress *) {
               devicesMayHaveChanged();
             });
    }
  }

  ~CoreAudioBackend() override
  {
    stop();
    unlisten(systemListeners);
    unlisten(deviceListeners);
    // whatever a listener block had queued has run once this returns
    dispatch_sync(queue, ^{
                  });
    dispatch_release(queue);
  }

  std::string apiId() const override
  {
    return "core";
  }

  std::string apiDisplayName() const override
  {
    return "CoreAudio";
  }

  const std::vector<AudioDevice> &devices() override
  {
    if (!listed) list();
    return deviceCache;
  }

  void refreshDevices() override
  {
    listed = false;
  }

 private:
  void list()
  {
    deviceCache = listCoreAudioDevices();
    listed = true;
    {
      std::lock_guard<std::mutex> guard(signatureLock);
      signature = signatureOf(deviceCache);
    }

    // a device's rates or channels changing, or it going away, without the device list
    // itself changing
    unlisten(deviceListeners);
    for (const auto &device : deviceCache)
    {
      const auto id{deviceWithUid(device.id)};
      if (id == kAudioObjectUnknown) continue;
      for (auto selector : {kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyDeviceIsAlive,
                            kAudioDevicePropertyAvailableNominalSampleRates})
      {
        listen(deviceListeners, id, address(selector), ^(UInt32, const AudioObjectPropertyAddress *) {
          devicesMayHaveChanged();
        });
      }
      listen(deviceListeners, id,
             address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeWildcard,
                     kAudioObjectPropertyElementWildcard),
             ^(UInt32, const AudioObjectPropertyAddress *) {
               devicesMayHaveChanged();
             });
    }
  }

 public:
  std::string defaultInputDevice() override
  {
    return uidOf(defaultDevice(true));
  }

  std::string defaultOutputDevice() override
  {
    return uidOf(defaultDevice(false));
  }

  bool open(const AudioStreamRequest &request, AudioStreamInfo &granted, std::string &error) override
  {
    stop();

    const bool useOutput{!request.outputDevice.empty() && request.outputChannels > 0};
    const bool useInput{!request.inputDevice.empty() && request.inputChannels > 0};
    if (!useOutput && !useInput)
    {
      error = "No audio device to open";
      return false;
    }

    outputDevice = useOutput ? deviceWithUid(request.outputDevice) : kAudioObjectUnknown;
    inputDevice = useInput ? deviceWithUid(request.inputDevice) : kAudioObjectUnknown;
    if (useOutput && outputDevice == kAudioObjectUnknown)
    {
      error = "The output device is not connected";
      return false;
    }
    if (useInput && inputDevice == kAudioObjectUnknown)
    {
      error = "The input device is not connected";
      return false;
    }

    // the rate: set on each real device, before an aggregate is built on them, so that
    // the aggregate never quietly resamples one of them (plan 13, 4.2 step 1)
    Float64 rate{static_cast<Float64>(request.sampleRate)};
    for (auto device : {outputDevice, inputDevice})
    {
      if (device == kAudioObjectUnknown) continue;
      if (!setRate(device, rate, error))
      {
        closeAll();
        return false;
      }
    }

    // one device, or an aggregate of two
    if (useOutput && useInput && outputDevice != inputDevice)
    {
      if (!createAggregate(error))
      {
        closeAll();
        return false;
      }
      ioDevice = aggregate;
      // sub-devices in the order output, then input: the output device's channels come
      // first in both directions (4.2 step 4)
      inputOffset = channelCount(outputDevice, true);
      outputOffset = 0;
    }
    else
    {
      ioDevice = useOutput ? outputDevice : inputDevice;
      inputOffset = 0;
      outputOffset = 0;
    }

    if ((useInput && !streamsAreFloat(ioDevice, true)) ||
        (useOutput && !streamsAreFloat(ioDevice, false)))
    {
      error = "The audio device does not deliver 32-bit float samples";
      closeAll();
      return false;
    }

    // the buffer size, per process, then what was granted
    UInt32 frames{request.bufferFrames > 0 ? request.bufferFrames : 256};
    setProperty(ioDevice, address(kAudioDevicePropertyBufferFrameSize), frames);
    getProperty(ioDevice, address(kAudioDevicePropertyBufferFrameSize), frames);
    if (frames == 0)
    {
      error = "The audio device has no buffer size";
      closeAll();
      return false;
    }

    inputChannels = useInput ? request.inputChannels : 0;
    outputChannels = useOutput ? request.outputChannels : 0;
    if (!mapChannels(true, request.inputFirstChannel, error) ||
        !mapChannels(false, request.outputFirstChannel, error))
    {
      closeAll();
      return false;
    }

    maxFrames = frames;
    inputScratch.assign(static_cast<size_t>(inputChannels) * maxFrames, 0.f);
    outputScratch.assign(static_cast<size_t>(outputChannels) * maxFrames, 0.f);
    for (uint32_t ch = 0; ch < inputChannels; ++ch) inputPointers[ch] = &inputScratch[ch * maxFrames];
    for (uint32_t ch = 0; ch < outputChannels; ++ch) outputPointers[ch] = &outputScratch[ch * maxFrames];

    if (AudioDeviceCreateIOProcID(ioDevice, &ioProc, this, &procId) != noErr || !procId)
    {
      error = "Unable to set up the audio device";
      closeAll();
      return false;
    }

    // the streams no channel is taken from are switched off: the input device's outputs
    // and the output device's inputs, in an aggregate (4.2 step 4)
    useStreams(true);
    useStreams(false);

    Float64 actualRate{rate};
    getProperty(ioDevice, address(kAudioDevicePropertyNominalSampleRate), actualRate);

    granted = {};
    granted.sampleRate = static_cast<uint32_t>(actualRate + 0.5);
    granted.bufferFrames = frames;
    granted.maxFramesPerCallback = frames;
    granted.inputChannels = inputChannels;
    granted.outputChannels = outputChannels;
    // each direction from its own real device, never from the aggregate (4.2 step 6)
    granted.inputLatency = useInput ? latencyOf(inputDevice, true) : 0;
    granted.outputLatency = useOutput ? latencyOf(outputDevice, false) : 0;

    streamRate = actualRate;
    opened = true;
    return true;
  }

  bool start(AudioStreamCallback &cb, std::string &error) override
  {
    if (!opened)
    {
      error = "No audio stream is open";
      return false;
    }

    callback = &cb;
    enabled = true;
    if (AudioDeviceStart(ioDevice, procId) != noErr)
    {
      error = "Unable to start the audio device";
      stop();
      return false;
    }
    started = true;

    watchRunningStream();
    return true;
  }

  void stop() override
  {
    unlisten(streamListeners);

    enabled = false;
    if (procId)
    {
      if (started) AudioDeviceStop(ioDevice, procId);
      AudioDeviceDestroyIOProcID(ioDevice, procId);
      procId = nullptr;
    }
    started = false;

    // AudioDeviceStop returns with the IOProc done when called from outside it; this is
    // for the case it does not
    for (int i = 0; i < 2000 && inCallback.load() > 0; ++i) usleep(1000);

    callback = nullptr;
    closeAll();
  }

  bool isOpen() const override
  {
    return opened;
  }

  bool isRunning() const override
  {
    return started;
  }

 private:
  static OSStatus ioProc(AudioObjectID, const AudioTimeStamp *, const AudioBufferList *inputData,
                         const AudioTimeStamp *, AudioBufferList *outputData, const AudioTimeStamp *,
                         void *clientData)
  {
    static_cast<CoreAudioBackend *>(clientData)->render(inputData, outputData);
    return noErr;
  }

  // [audio-thread]
  void render(const AudioBufferList *inputData, AudioBufferList *outputData)
  {
    struct Guard
    {
      std::atomic<int> &count;
      explicit Guard(std::atomic<int> &c) : count(c)
      {
        ++count;
      }
      ~Guard()
      {
        --count;
      }
    } guard(inCallback);

    // what no channel of ours is mapped to stays silent
    for (UInt32 b = 0; outputData && b < outputData->mNumberBuffers; ++b)
    {
      if (outputData->mBuffers[b].mData)
        memset(outputData->mBuffers[b].mData, 0, outputData->mBuffers[b].mDataByteSize);
    }

    auto *cb{callback.load()};
    if (!enabled || !cb) return;

    const auto frames{framesIn(inputData, outputData)};
    for (uint32_t done = 0; done < frames;)
    {
      const auto n{std::min(frames - done, maxFrames)};

      for (uint32_t ch = 0; ch < inputChannels; ++ch)
      {
        const auto &map{inputMap[ch]};
        const auto *buffer{inputData && map.buffer < inputData->mNumberBuffers
                               ? static_cast<const float *>(inputData->mBuffers[map.buffer].mData)
                               : nullptr};
        auto *into{inputPointers[ch]};
        if (!buffer)
        {
          memset(into, 0, n * sizeof(float));
          continue;
        }
        for (uint32_t i = 0; i < n; ++i) into[i] = buffer[(done + i) * map.stride + map.offset];
      }

      cb->process(inputChannels ? inputPointers : nullptr, inputChannels,
                  outputChannels ? outputPointers : nullptr, outputChannels, n);

      for (uint32_t ch = 0; ch < outputChannels; ++ch)
      {
        const auto &map{outputMap[ch]};
        auto *buffer{outputData && map.buffer < outputData->mNumberBuffers
                         ? static_cast<float *>(outputData->mBuffers[map.buffer].mData)
                         : nullptr};
        if (!buffer) continue;
        const auto *from{outputPointers[ch]};
        for (uint32_t i = 0; i < n; ++i) buffer[(done + i) * map.stride + map.offset] = from[i];
      }

      done += n;
    }
  }

  // the frames in this cycle, from whichever side has a buffer of ours
  uint32_t framesIn(const AudioBufferList *inputData, const AudioBufferList *outputData) const
  {
    if (outputChannels > 0 && outputData)
    {
      const auto &map{outputMap[0]};
      if (map.buffer < outputData->mNumberBuffers &&
          outputData->mBuffers[map.buffer].mNumberChannels > 0)
        return outputData->mBuffers[map.buffer].mDataByteSize /
               (sizeof(float) * outputData->mBuffers[map.buffer].mNumberChannels);
    }
    if (inputChannels > 0 && inputData)
    {
      const auto &map{inputMap[0]};
      if (map.buffer < inputData->mNumberBuffers && inputData->mBuffers[map.buffer].mNumberChannels > 0)
        return inputData->mBuffers[map.buffer].mDataByteSize /
               (sizeof(float) * inputData->mBuffers[map.buffer].mNumberChannels);
    }
    return 0;
  }

  // where each opened channel lives in the io device's buffer list
  bool mapChannels(bool input, uint32_t firstChannel, std::string &error)
  {
    auto &map{input ? inputMap : outputMap};
    const auto count{input ? inputChannels : outputChannels};
    const auto offset{input ? inputOffset : outputOffset};
    const auto streams{streamChannels(ioDevice, input)};

    for (uint32_t ch = 0; ch < count; ++ch)
    {
      auto channel{offset + firstChannel + ch};
      bool found{false};
      for (uint32_t b = 0; b < streams.size(); ++b)
      {
        if (channel < streams[b])
        {
          map[ch] = {b, channel, streams[b]};
          found = true;
          break;
        }
        channel -= streams[b];
      }
      if (!found)
      {
        error = input ? "The input device does not have the channels asked for"
                      : "The output device does not have the channels asked for";
        return false;
      }
    }
    return true;
  }

  // switches off the io device's streams of one direction that no opened channel is in
  void useStreams(bool input)
  {
    const auto scope{scopeOf(input)};
    auto where{address(kAudioDevicePropertyIOProcStreamUsage, scope)};
    auto bytes{getBytes(ioDevice, where)};
    if (bytes.size() < sizeof(AudioHardwareIOProcStreamUsage)) return;

    // the property is read for one io proc, named in the struct
    auto *usage{reinterpret_cast<AudioHardwareIOProcStreamUsage *>(bytes.data())};
    usage->mIOProc = reinterpret_cast<void *>(procId);
    UInt32 size{static_cast<UInt32>(bytes.size())};
    if (AudioObjectGetPropertyData(ioDevice, &where, 0, nullptr, &size, usage) != noErr) return;

    const auto &map{input ? inputMap : outputMap};
    const auto count{input ? inputChannels : outputChannels};
    for (UInt32 s = 0; s < usage->mNumberStreams; ++s)
    {
      bool used{false};
      for (uint32_t ch = 0; ch < count; ++ch) used = used || map[ch].buffer == s;
      usage->mStreamIsOn[s] = used ? 1 : 0;
    }
    usage->mIOProc = reinterpret_cast<void *>(procId);
    AudioObjectSetPropertyData(ioDevice, &where, 0, nullptr, size, usage);
  }

  // sets a device's nominal rate and waits for it to take; true if it runs at that rate
  // already. a device whose rate is not ours to set is left at its own, and the stream
  // takes that
  bool setRate(AudioDeviceID device, Float64 &rate, std::string &error)
  {
    Float64 current{0};
    getProperty(device, address(kAudioDevicePropertyNominalSampleRate), current);
    if (rate <= 0 || std::abs(current - rate) < 1)
    {
      if (rate <= 0) rate = current;
      return true;
    }

    AudioDevice described;
    if (describe(device, kAudioObjectUnknown, kAudioObjectUnknown, described) &&
        !described.sampleRateSettable)
    {
      // two devices: both have to run at the one rate
      if (outputDevice != kAudioObjectUnknown && inputDevice != kAudioObjectUnknown &&
          outputDevice != inputDevice)
      {
        error = nameOf(device) + " runs at " + std::to_string(static_cast<int>(current)) +
                " Hz only, and cannot be set to " + std::to_string(static_cast<int>(rate)) + " Hz";
        return false;
      }
      rate = current;
      return true;
    }

    // the listener says when it has taken, which may be some time after the set returns
    auto changed{dispatch_semaphore_create(0)};
    const auto where{address(kAudioDevicePropertyNominalSampleRate)};
    AudioObjectPropertyListenerBlock block{Block_copy(^(UInt32, const AudioObjectPropertyAddress *) {
      dispatch_semaphore_signal(changed);
    })};
    AudioObjectAddPropertyListenerBlock(device, &where, queue, block);

    bool ok{setProperty(device, where, rate)};
    for (int i = 0; ok && i < 20; ++i)
    {
      getProperty(device, where, current);
      if (std::abs(current - rate) < 1) break;
      dispatch_semaphore_wait(changed, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC));
    }

    AudioObjectRemovePropertyListenerBlock(device, &where, queue, block);
    Block_release(block);
    dispatch_release(changed);

    getProperty(device, where, current);
    if (!ok || std::abs(current - rate) >= 1)
    {
      error =
          "Unable to set " + nameOf(device) + " to " + std::to_string(static_cast<int>(rate)) + " Hz";
      return false;
    }
    return true;
  }

  // a private aggregate of the output and the input device (plan 13, 4.2 step 2-3)
  bool createAggregate(std::string &error)
  {
    const auto outputUid{uidOf(outputDevice)};
    const auto inputUid{uidOf(inputDevice)};

    // drift compensation on the input, unless both run off one clock
    UInt32 outputDomain{0}, inputDomain{0};
    getProperty(outputDevice, address(kAudioDevicePropertyClockDomain), outputDomain);
    getProperty(inputDevice, address(kAudioDevicePropertyClockDomain), inputDomain);
    const bool drift{outputDomain == 0 || outputDomain != inputDomain};

    // the keys spelled out, so that no newer sdk is needed for them
    auto description{CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks)};
    setString(description, "uid", aggregateUidPrefix + newUuid());
    setString(description, "name", "clap-wrapper standalone");
    setNumber(description, "private", 1);
    setNumber(description, "stacked", 0);
    setString(description, "master", outputUid);

    auto subDevices{CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks)};
    for (const auto *uid : {&outputUid, &inputUid})
    {
      auto sub{CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks)};
      setString(sub, "uid", *uid);
      if (uid == &inputUid && drift)
      {
        setNumber(sub, "drift", 1);
        setNumber(sub, "drift quality", 0x60);  // kAudioAggregateDriftCompensationHighQuality
      }
      CFArrayAppendValue(subDevices, sub);
      CFRelease(sub);
    }
    auto subKey{cfString("subdevices")};
    CFDictionarySetValue(description, subKey, subDevices);
    CFRelease(subKey);
    CFRelease(subDevices);

    const auto status{AudioHardwareCreateAggregateDevice(description, &aggregate)};
    CFRelease(description);
    if (status != noErr || aggregate == kAudioObjectUnknown)
    {
      aggregate = kAudioObjectUnknown;
      error = "Unable to combine " + nameOf(outputDevice) + " and " + nameOf(inputDevice);
      return false;
    }

    // the id returned is usable at once, even before the device list has it (4.1); it
    // has to hold both devices, or one of them went away on the way
    const auto active{
        getArray<AudioObjectID>(aggregate, address(kAudioAggregateDevicePropertyActiveSubDeviceList))};
    if (active.size() != 2 || channelCount(aggregate, true) !=
                                  channelCount(outputDevice, true) + channelCount(inputDevice, true))
    {
      error = "Unable to combine " + nameOf(outputDevice) + " and " + nameOf(inputDevice);
      return false;
    }

    LOGINFO("Combined '{}' (clock) and '{}' (drift compensation {}) for input and output",
            nameOf(outputDevice), nameOf(inputDevice), drift ? "on" : "off");
    return true;
  }

  void closeAll()
  {
    if (aggregate != kAudioObjectUnknown)
    {
      AudioHardwareDestroyAggregateDevice(aggregate);
      aggregate = kAudioObjectUnknown;
    }
    ioDevice = outputDevice = inputDevice = kAudioObjectUnknown;
    inputChannels = outputChannels = 0;
    opened = false;
  }

  // a device of the running stream going away, or another application changing its rate,
  // stops the stream; the host closes it (4.2 step 7)
  void watchRunningStream()
  {
    for (auto device : {outputDevice, inputDevice, aggregate})
    {
      if (device == kAudioObjectUnknown) continue;

      listen(streamListeners, device, address(kAudioDevicePropertyDeviceIsAlive),
             ^(UInt32, const AudioObjectPropertyAddress *) {
               UInt32 alive{1};
               getProperty(device, address(kAudioDevicePropertyDeviceIsAlive), alive);
               if (!alive) streamStopped(nameOf(device) + " is no longer available");
             });
      listen(streamListeners, device, address(kAudioDevicePropertyNominalSampleRate),
             ^(UInt32, const AudioObjectPropertyAddress *) {
               Float64 rate{0};
               getProperty(device, address(kAudioDevicePropertyNominalSampleRate), rate);
               if (rate > 0 && std::abs(rate - streamRate) >= 1)
                 streamStopped(nameOf(device) + " was switched to " +
                               std::to_string(static_cast<int>(rate)) + " Hz by another application");
             });
    }

    listen(streamListeners, ioDevice, address(kAudioDeviceProcessorOverload),
           ^(UInt32, const AudioObjectPropertyAddress *) {
             const auto count{++overloads};
             if (count == 1 || count % 100 == 0) LOGINFO("[WARNING] Audio overload, {} so far", count);
           });
  }

  // [listener-queue]
  void streamStopped(const std::string &message)
  {
    if (!enabled.exchange(false)) return;
    if (onEvent) onEvent(AudioBackendEvent::StreamStopped, message);
  }

  // [listener-queue] a device came or went, or changed what it offers. only a list that
  // reads differently now is news: building and removing our own aggregate tells every
  // listener in this process, too (4.2 step 5)
  void devicesMayHaveChanged()
  {
    const auto now{signatureOf(listCoreAudioDevices())};
    {
      std::lock_guard<std::mutex> guard(signatureLock);
      if (now == signature) return;
      signature = now;
    }
    if (onEvent) onEvent(AudioBackendEvent::DevicesChanged, {});
  }

  static std::string signatureOf(const std::vector<AudioDevice> &list)
  {
    std::string res;
    for (const auto &d : list)
    {
      res += d.id + '\x1f' + d.name + '\x1f' + std::to_string(d.inputChannels) + '/' +
             std::to_string(d.outputChannels) + '@' + std::to_string(d.currentSampleRate) + ':';
      for (auto r : d.sampleRates) res += std::to_string(r) + ',';
      res += (d.isDefaultInput ? "i" : "") + std::string(d.isDefaultOutput ? "o" : "") + '\x1e';
    }
    return res;
  }

  void listen(std::vector<Listener> &into, AudioObjectID object, AudioObjectPropertyAddress where,
              AudioObjectPropertyListenerBlock block)
  {
    Listener listener{object, where, Block_copy(block)};
    if (AudioObjectAddPropertyListenerBlock(object, &listener.where, queue, listener.block) == noErr)
      into.push_back(listener);
    else
      Block_release(listener.block);
  }

  void unlisten(std::vector<Listener> &from)
  {
    for (auto &listener : from)
    {
      AudioObjectRemovePropertyListenerBlock(listener.object, &listener.where, queue, listener.block);
      Block_release(listener.block);
    }
    from.clear();
  }

  struct ChannelMap
  {
    uint32_t buffer{0}, offset{0}, stride{1};
  };
  static constexpr uint32_t maxChannels{64};

  dispatch_queue_t queue{nullptr};
  std::vector<Listener> systemListeners, deviceListeners, streamListeners;

  std::vector<AudioDevice> deviceCache;
  bool listed{false};
  std::mutex signatureLock;
  std::string signature;

  AudioDeviceID outputDevice{kAudioObjectUnknown}, inputDevice{kAudioObjectUnknown};
  AudioDeviceID aggregate{kAudioObjectUnknown}, ioDevice{kAudioObjectUnknown};
  AudioDeviceIOProcID procId{nullptr};
  uint32_t inputOffset{0}, outputOffset{0};
  Float64 streamRate{0};
  bool opened{false}, started{false};

  uint32_t inputChannels{0}, outputChannels{0}, maxFrames{0};
  ChannelMap inputMap[maxChannels]{}, outputMap[maxChannels]{};
  std::vector<float> inputScratch, outputScratch;
  float *inputPointers[maxChannels]{};
  float *outputPointers[maxChannels]{};

  std::atomic<AudioStreamCallback *> callback{nullptr};
  std::atomic<bool> enabled{false};
  std::atomic<int> inCallback{0};
  std::atomic<uint32_t> overloads{0};
};
}  // namespace

std::vector<AudioDevice> listCoreAudioDevices()
{
  const auto defaultInput{defaultDevice(true)};
  const auto defaultOutput{defaultDevice(false)};

  std::vector<AudioDevice> res;
  for (auto id : allDevices())
  {
    AudioDevice device;
    if (describe(id, defaultInput, defaultOutput, device)) res.push_back(std::move(device));
  }
  return res;
}

#if !CLAP_WRAPPER_STANDALONE_RTAUDIO
std::unique_ptr<AudioBackend> createCoreAudioBackend()
{
  return std::make_unique<CoreAudioBackend>();
}
#endif
}  // namespace freeaudio::clap_wrapper::standalone
