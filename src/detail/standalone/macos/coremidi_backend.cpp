#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "detail/standalone/standalone_backends.h"
#include "detail/standalone/standalone_details.h"

/*
 * CoreMIDI, for the standalone on macOS. A port's id is its kMIDIPropertyUniqueID; its
 * name is the one RtMidi gave it, which settings files from before hold.
 *
 * The packet list calls are the pre-UMP ones, since clap-wrapper's floor is macOS 10.13;
 * newer SDKs mark some of them deprecated.
 */

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
std::string toString(CFStringRef text)
{
  if (!text) return {};
  std::vector<char> buffer(static_cast<size_t>(CFStringGetLength(text)) * 3 + 1, 0);
  if (!CFStringGetCString(text, buffer.data(), static_cast<CFIndex>(buffer.size()),
                          kCFStringEncodingUTF8))
    return {};
  return buffer.data();
}

std::string stringProperty(MIDIObjectRef object, CFStringRef property)
{
  CFStringRef text{nullptr};
  if (MIDIObjectGetStringProperty(object, property, &text) != noErr || !text) return {};
  auto res{toString(text)};
  CFRelease(text);
  return res;
}

std::string trimSpaces(const std::string &text)
{
  const auto b{text.find_first_not_of(' ')};
  if (b == std::string::npos) return {};
  return text.substr(b, text.find_last_not_of(' ') - b + 1);
}

// RtMidi's CreateEndpointName, which it has from PortMidi: the endpoint's name, after the
// device's unless it starts with it already; the device's alone for an external device
// with one entity
std::string endpointName(MIDIEndpointRef endpoint, bool isExternal)
{
  auto result{trimSpaces(stringProperty(endpoint, kMIDIPropertyName))};

  MIDIEntityRef entity{0};
  MIDIEndpointGetEntity(endpoint, &entity);
  if (entity == 0) return result;  // probably virtual

  if (result.empty()) result = stringProperty(entity, kMIDIPropertyName);

  MIDIDeviceRef device{0};
  MIDIEntityGetDevice(entity, &device);
  if (device == 0) return result;

  const auto deviceName{stringProperty(device, kMIDIPropertyName)};
  if (result.empty()) return deviceName;
  if (isExternal && MIDIDeviceGetNumberOfEntities(device) < 2) return deviceName;
  if (deviceName.empty()) return result;
  if (result.compare(0, deviceName.size(), deviceName) != 0) result = deviceName + " " + result;
  return result;
}

// RtMidi's CreateConnectedEndpointName: the names of the external devices connected to
// the endpoint in Audio MIDI Setup, or the endpoint's own
std::string connectedEndpointName(MIDIEndpointRef endpoint)
{
  std::string result;
  CFDataRef connections{nullptr};
  MIDIObjectGetDataProperty(endpoint, kMIDIPropertyConnectionUniqueID, &connections);
  if (connections)
  {
    const auto count{CFDataGetLength(connections) / static_cast<CFIndex>(sizeof(MIDIUniqueID))};
    const auto *ids{reinterpret_cast<const SInt32 *>(CFDataGetBytePtr(connections))};
    for (CFIndex i = 0; i < count; ++i)
    {
      MIDIObjectRef object{0};
      MIDIObjectType type{};
      if (MIDIObjectFindByUniqueID(
              static_cast<MIDIUniqueID>(CFSwapInt32BigToHost(static_cast<uint32_t>(ids[i]))), &object,
              &type) != noErr)
        continue;

      const auto name{type == kMIDIObjectType_ExternalSource ||
                              type == kMIDIObjectType_ExternalDestination
                          ? endpointName(static_cast<MIDIEndpointRef>(object), true)
                          : stringProperty(object, kMIDIPropertyName)};
      if (name.empty()) continue;
      if (!result.empty()) result += ", ";
      result += name;
    }
    CFRelease(connections);
  }
  if (!result.empty()) return result;
  return endpointName(endpoint, false);
}

MIDIUniqueID uniqueIdOf(MIDIObjectRef object)
{
  SInt32 id{0};
  MIDIObjectGetIntegerProperty(object, kMIDIPropertyUniqueID, &id);
  return id;
}

// a unique id for a virtual port that stays the same from one launch to the next, so that
// another application's connection to it survives a relaunch: from its name
MIDIUniqueID stableIdFor(const std::string &name)
{
  uint32_t hash{2166136261u};
  for (auto c : name) hash = (hash ^ static_cast<uint8_t>(c)) * 16777619u;
  // never 0, which is no id, and kept off the negative range other code may treat oddly
  return static_cast<MIDIUniqueID>((hash & 0x7ffffffe) | 1);
}

// splits a byte stream into complete messages, one per source: running status expanded,
// sysex reassembled across packets, realtime bytes passed on where they arrive
struct MidiParser
{
  static constexpr size_t maxSysex{64 * 1024};

  void feed(const uint8_t *data, size_t size, const std::function<void(const uint8_t *, size_t)> &out)
  {
    for (size_t i = 0; i < size; ++i)
    {
      const auto b{data[i]};

      if (b >= 0xF8)
      {
        out(&b, 1);
        continue;
      }

      if (inSysex)
      {
        if (b < 0x80)
        {
          if (sysex.size() < maxSysex)
            sysex.push_back(b);
          else
            sysexTooLong = true;
          continue;
        }
        if (b == 0xF7)
        {
          sysex.push_back(b);
          if (!sysexTooLong) out(sysex.data(), sysex.size());
        }
        // any other status ends a sysex that never got its F7, which is dropped
        inSysex = false;
        sysex.clear();
        if (b == 0xF7) continue;
      }

      if (b == 0xF0)
      {
        inSysex = true;
        sysexTooLong = false;
        sysex.clear();
        sysex.push_back(b);
        running = 0;
        continue;
      }

      if (b >= 0x80)
      {
        running = b < 0xF0 ? b : 0;
        message[0] = b;
        have = 1;
        need = shortMidiMessageSize(b);
        if (need <= 1)
        {
          if (need == 1) out(message, 1);
          have = 0;
        }
        continue;
      }

      // a data byte
      if (have == 0)
      {
        if (!running) continue;
        message[0] = running;
        have = 1;
        need = shortMidiMessageSize(running);
      }
      message[have++] = b;
      if (have >= need)
      {
        out(message, need);
        have = 0;
      }
    }
  }

  uint8_t running{0};
  uint8_t message[3]{};
  size_t have{0}, need{0};
  bool inSysex{false}, sysexTooLong{false};
  std::vector<uint8_t> sysex;
};

class CoreMidiBackend final : public MidiBackend
{
 public:
  CoreMidiBackend()
  {
    auto name{CFSTR("clap-wrapper standalone")};
    // the notify block runs on the run loop of the thread that made the client, the main one
    if (MIDIClientCreateWithBlock(name, &client, ^(const MIDINotification *notification) {
          switch (notification->messageID)
          {
            case kMIDIMsgSetupChanged:
            case kMIDIMsgObjectAdded:
            case kMIDIMsgObjectRemoved:
              if (onPortsChanged) onPortsChanged();
              break;
            default:
              break;
          }
        }) != noErr)
    {
      LOGINFO("[ERROR] Unable to create a CoreMIDI client");
      client = 0;
      return;
    }

    MIDIInputPortCreateWithBlock(client, CFSTR("Input"), &inputPort,
                                 ^(const MIDIPacketList *packets, void *source) {
                                   receive(packets, static_cast<MidiParser *>(source));
                                 });
    MIDIOutputPortCreate(client, CFSTR("Output"), &outputPort);
  }

  ~CoreMidiBackend() override
  {
    openInputs({});
    openOutputs({});
    setVirtualInput({});
    setVirtualOutput({});
    // disposing of the client disposes of its ports
    if (client) MIDIClientDispose(client);
  }

  std::string apiId() const override
  {
    return "coremidi";
  }

  std::vector<MidiPort> inputs() override
  {
    std::vector<MidiPort> res;
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
    {
      const auto source{MIDIGetSource(i)};
      if (source == 0 || source == virtualSource) continue;
      res.push_back(portFor(source));
    }
    makeNamesUnique(res);
    return res;
  }

  std::vector<MidiPort> outputs() override
  {
    std::vector<MidiPort> res;
    for (ItemCount i = 0; i < MIDIGetNumberOfDestinations(); ++i)
    {
      const auto destination{MIDIGetDestination(i)};
      if (destination == 0 || destination == virtualDestination) continue;
      res.push_back(portFor(destination));
    }
    makeNamesUnique(res);
    return res;
  }

  std::vector<std::string> openInputs(const std::vector<std::string> &ids) override
  {
    // a read block may still be running on a parser, so they are kept, not freed
    for (auto &[source, parser] : connected)
    {
      MIDIPortDisconnectSource(inputPort, source);
      retired.push_back(std::move(parser));
    }
    connected.clear();

    std::vector<std::string> opened;
    if (!inputPort) return opened;

    for (const auto &id : ids)
    {
      const auto source{endpointWithId(id, true)};
      if (source == 0) continue;

      auto parser{std::make_unique<MidiParser>()};
      if (MIDIPortConnectSource(inputPort, source, parser.get()) != noErr) continue;
      LOGDETAIL("MIDI: opening '{}'", connectedEndpointName(source));
      connected.emplace_back(source, std::move(parser));
      opened.push_back(id);
    }
    return opened;
  }

  std::vector<std::string> openOutputs(const std::vector<std::string> &ids) override
  {
    std::vector<MIDIEndpointRef> destinations;
    std::vector<std::string> opened;
    for (const auto &id : ids)
    {
      const auto destination{endpointWithId(id, false)};
      if (destination == 0) continue;
      destinations.push_back(destination);
      opened.push_back(id);
    }

    std::lock_guard<std::mutex> guard(outputLock);
    outputDestinations = destinations;
    return opened;
  }

  bool supportsVirtualPorts() const override
  {
    return true;
  }

  bool setVirtualInput(const std::string &name) override
  {
    if (virtualDestination)
    {
      MIDIEndpointDispose(virtualDestination);
      virtualDestination = 0;
      retired.push_back(std::move(virtualParser));
    }
    if (name.empty() || !client) return name.empty();

    virtualParser = std::make_unique<MidiParser>();
    auto *parser{virtualParser.get()};
    auto cfName{CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8)};
    const auto status{MIDIDestinationCreateWithBlock(client, cfName, &virtualDestination,
                                                     ^(const MIDIPacketList *packets, void *) {
                                                       receive(packets, parser);
                                                     })};
    CFRelease(cfName);
    if (status != noErr)
    {
      virtualDestination = 0;
      virtualParser.reset();
      return false;
    }
    MIDIObjectSetIntegerProperty(virtualDestination, kMIDIPropertyUniqueID, stableIdFor(name + " in"));
    return true;
  }

  bool setVirtualOutput(const std::string &name) override
  {
    std::lock_guard<std::mutex> guard(outputLock);
    if (virtualSource)
    {
      MIDIEndpointDispose(virtualSource);
      virtualSource = 0;
    }
    if (name.empty() || !client) return name.empty();

    auto cfName{CFStringCreateWithCString(kCFAllocatorDefault, name.c_str(), kCFStringEncodingUTF8)};
    const auto status{MIDISourceCreate(client, cfName, &virtualSource)};
    CFRelease(cfName);
    if (status != noErr)
    {
      virtualSource = 0;
      return false;
    }
    MIDIObjectSetIntegerProperty(virtualSource, kMIDIPropertyUniqueID, stableIdFor(name + " out"));
    return true;
  }

  void send(const uint8_t *data, size_t size) override
  {
    if (!data || size == 0 || size > MidiParser::maxSysex) return;

    // one packet holds the whole message, sysex included
    sendBuffer.resize(size + 1024);
    auto *list{reinterpret_cast<MIDIPacketList *>(sendBuffer.data())};
    auto *packet{MIDIPacketListInit(list)};
    packet = MIDIPacketListAdd(list, sendBuffer.size(), packet, 0, size, data);
    if (!packet) return;

    std::lock_guard<std::mutex> guard(outputLock);
    for (auto destination : outputDestinations) MIDISend(outputPort, destination, list);
    if (virtualSource) MIDIReceived(virtualSource, list);
  }

 private:
  // [coremidi-thread]
  void receive(const MIDIPacketList *packets, MidiParser *parser)
  {
    if (!packets || !parser || !onMessage) return;

    const auto out{[this](const uint8_t *data, size_t size) { onMessage(data, size); }};
    const auto *packet{&packets->packet[0]};
    for (UInt32 i = 0; i < packets->numPackets; ++i)
    {
      parser->feed(packet->data, packet->length, out);
      packet = MIDIPacketNext(packet);
    }
  }

  MidiPort portFor(MIDIEndpointRef endpoint)
  {
    MidiPort port;
    port.id = std::to_string(uniqueIdOf(endpoint));
    port.legacyName = connectedEndpointName(endpoint);
    port.name = port.legacyName.empty() ? stringProperty(endpoint, kMIDIPropertyName) : port.legacyName;
    return port;
  }

  // two ports of one name get " (2)" and so on, as the audio menus do
  static void makeNamesUnique(std::vector<MidiPort> &ports)
  {
    std::map<std::string, int> seen;
    for (auto &port : ports)
    {
      const auto count{++seen[port.name]};
      if (count > 1) port.name += " (" + std::to_string(count) + ")";
    }
  }

  MIDIEndpointRef endpointWithId(const std::string &id, bool source)
  {
    const auto count{source ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations()};
    for (ItemCount i = 0; i < count; ++i)
    {
      const auto endpoint{source ? MIDIGetSource(i) : MIDIGetDestination(i)};
      if (endpoint == 0 || endpoint == virtualSource || endpoint == virtualDestination) continue;
      if (std::to_string(uniqueIdOf(endpoint)) == id) return endpoint;
    }
    return 0;
  }

  MIDIClientRef client{0};
  MIDIPortRef inputPort{0}, outputPort{0};

  // the sources the input port is connected to, each with the parser its packets go through
  std::vector<std::pair<MIDIEndpointRef, std::unique_ptr<MidiParser>>> connected;

  MIDIEndpointRef virtualDestination{0}, virtualSource{0};
  std::unique_ptr<MidiParser> virtualParser;

  // parsers of sources no longer connected, which a read block may still be in
  std::vector<std::unique_ptr<MidiParser>> retired;

  // the sender thread's, against openOutputs() and setVirtualOutput() on the main thread
  std::mutex outputLock;
  std::vector<MIDIEndpointRef> outputDestinations;
  std::vector<uint8_t> sendBuffer;
};
}  // namespace

std::unique_ptr<MidiBackend> createCoreMidiBackend()
{
  return std::make_unique<CoreMidiBackend>();
}
}  // namespace freeaudio::clap_wrapper::standalone

#ifdef __clang__
#pragma clang diagnostic pop
#endif
