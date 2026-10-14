

#include <Foundation/Foundation.h>
#include <CoreAudio/CoreAudio.h>
#include "detail/standalone/standalone_host.h"

#include <algorithm>

namespace
{
// kAudioObjectPropertyElementMain, which was kAudioObjectPropertyElementMaster before
// macOS 12; both are 0
constexpr AudioObjectPropertyElement elementMain{0};

template <typename T>
bool getProperty(AudioObjectID id, AudioObjectPropertySelector selector, T &value)
{
  AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, elementMain};
  UInt32 size = sizeof(T);
  return AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &value) == noErr;
}

uint32_t inputChannels(AudioObjectID id)
{
  AudioObjectPropertyAddress address{kAudioDevicePropertyStreamConfiguration,
                                     kAudioDevicePropertyScopeInput, elementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(id, &address, 0, nullptr, &size) != noErr || size == 0) return 0;

  std::vector<uint8_t> storage(size);
  auto *buffers = reinterpret_cast<AudioBufferList *>(storage.data());
  if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, buffers) != noErr) return 0;

  uint32_t channels{0};
  for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) channels += buffers->mBuffers[i].mNumberChannels;
  return channels;
}

std::string toString(CFStringRef text, CFStringEncoding encoding)
{
  std::vector<char> buffer((size_t)CFStringGetLength(text) * 3 + 1, 0);
  if (!CFStringGetCString(text, buffer.data(), (CFIndex)buffer.size(), encoding)) return {};
  return buffer.data();
}

// the name rtaudio gives a coreaudio device - "manufacturer: name" - in the encoding it
// converts with, which is the system encoding unless it was built with UNICODE
std::string rtAudioName(AudioObjectID id, CFStringEncoding encoding)
{
  CFStringRef manufacturer{nullptr}, name{nullptr};
  if (!getProperty(id, kAudioObjectPropertyManufacturer, manufacturer)) return {};
  if (!getProperty(id, kAudioObjectPropertyName, name))
  {
    CFRelease(manufacturer);
    return {};
  }
  auto result = toString(manufacturer, encoding) + ": " + toString(name, encoding);
  CFRelease(manufacturer);
  CFRelease(name);
  return result;
}

bool isBluetooth(UInt32 transport)
{
  return transport == kAudioDeviceTransportTypeBluetooth ||
         transport == kAudioDeviceTransportTypeBluetoothLE;
}

// a microphone the user is unlikely to mean when an interface is plugged in: the
// built-in one, a bluetooth headset, or an iphone's (continuity, wired and wireless)
bool isMicrophone(UInt32 transport)
{
  return transport == kAudioDeviceTransportTypeBuiltIn || isBluetooth(transport) ||
         transport == 'ccwd' || transport == 'ccwl';
}

std::vector<AudioDeviceID> allDevices()
{
  AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     elementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr)
    return {};
  std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                 devices.data()) != noErr)
    return {};
  devices.resize(size / sizeof(AudioDeviceID));
  return devices;
}

// the coreaudio device rtaudio calls by this name, in either encoding it may have used
AudioDeviceID deviceNamed(const std::string &name)
{
  if (name.empty()) return kAudioObjectUnknown;

  for (auto id : allDevices())
  {
    for (auto encoding : {CFStringGetSystemEncoding(), (CFStringEncoding)kCFStringEncodingUTF8})
    {
      if (rtAudioName(id, encoding) == name) return id;
    }
  }
  return kAudioObjectUnknown;
}

// the transport type of that device; false if there is none
bool transportNamed(const std::string &name, UInt32 &transport)
{
  auto id{deviceNamed(name)};
  return id != kAudioObjectUnknown && getProperty(id, kAudioDevicePropertyTransportType, transport);
}

// hardware an instrument or a line is plugged into. not virtual or aggregate devices,
// which route other applications' audio or combine devices the user built themselves
bool isInterface(UInt32 transport)
{
  return transport == kAudioDeviceTransportTypeUSB ||
         transport == kAudioDeviceTransportTypeThunderbolt ||
         transport == kAudioDeviceTransportTypeFireWire || transport == kAudioDeviceTransportTypePCI ||
         transport == kAudioDeviceTransportTypeAVB;
}
}  // namespace

namespace freeaudio::clap_wrapper::standalone
{
std::vector<std::string> preferredFirstRunInputs()
{
  AudioDeviceID defaultInput{kAudioObjectUnknown};
  UInt32 transport{0};
  if (!getProperty(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice, defaultInput) ||
      !getProperty(defaultInput, kAudioDevicePropertyTransportType, transport) ||
      !isMicrophone(transport))
    return {};

  // the interface with the most inputs first: a usb microphone has one, an interface
  // two or more
  std::vector<std::pair<uint32_t, AudioDeviceID>> interfaces;
  for (auto id : allDevices())
  {
    UInt32 t{0};
    if (!getProperty(id, kAudioDevicePropertyTransportType, t) || !isInterface(t)) continue;
    if (auto channels = inputChannels(id); channels > 0) interfaces.emplace_back(channels, id);
  }
  std::stable_sort(interfaces.begin(), interfaces.end(),
                   [](const auto &a, const auto &b) { return a.first > b.first; });

  std::vector<std::string> names;
  for (const auto &interface : interfaces)
  {
    for (auto encoding : {CFStringGetSystemEncoding(), (CFStringEncoding)kCFStringEncodingUTF8})
    {
      auto name = rtAudioName(interface.second, encoding);
      if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
        names.push_back(name);
    }
  }
  return names;
}

bool isMicrophoneAudioDevice(const std::string &name)
{
  UInt32 transport{0};
  return transportNamed(name, transport) && isMicrophone(transport);
}

bool isBluetoothAudioDevice(const std::string &name)
{
  UInt32 transport{0};
  return transportNamed(name, transport) && isBluetooth(transport);
}

std::pair<uint32_t, uint32_t> bufferFrameSizeRange(const std::string &name)
{
  AudioValueRange range{};
  auto id{deviceNamed(name)};
  if (id == kAudioObjectUnknown || !getProperty(id, kAudioDevicePropertyBufferFrameSizeRange, range) ||
      range.mMaximum < range.mMinimum || range.mMinimum < 0)
    return {0, 0};
  return {static_cast<uint32_t>(range.mMinimum), static_cast<uint32_t>(range.mMaximum)};
}

std::optional<fs::path> getStandaloneSettingsPath()
{
  auto *fileManager = [NSFileManager defaultManager];
  auto *resultURLs = [fileManager URLsForDirectory:NSApplicationSupportDirectory
                                         inDomains:NSUserDomainMask];
  if (resultURLs)
  {
    auto *u = [resultURLs objectAtIndex:0];
    return fs::path{[u fileSystemRepresentation]} / "clap-wrapper-standalone";
  }
  return std::nullopt;
}
}  // namespace freeaudio::clap_wrapper::standalone
