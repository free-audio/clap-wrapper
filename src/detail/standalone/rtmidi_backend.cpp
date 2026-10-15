#include "standalone_backends.h"
#include "standalone_details.h"

#include <algorithm>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtMidi.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/*
 * RtMidi behind the MidiBackend interface, with CLAP_WRAPPER_STANDALONE_RTMIDI: what
 * CLAP_WRAPPER_STANDALONE_RTAUDIO builds, and Linux always. A port's id is RtMidi's name
 * for it, which is also what settings files from before the native backends hold.
 */

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// rtmidi's index of the port with this name, -1 if it has none
int portIndex(RtMidi &midi, const std::string &name)
{
  for (unsigned int i = 0; i < midi.getPortCount(); ++i)
    if (midi.getPortName(i) == name) return (int)i;
  return -1;
}

class RtMidiBackend final : public MidiBackend
{
 public:
  std::string apiId() const override
  {
    std::vector<RtMidi::Api> apis;
    RtMidi::getCompiledApi(apis);
    return apis.empty() ? std::string{"rtmidi"} : RtMidi::getApiName(apis.front());
  }

  std::vector<MidiPort> inputs() override
  {
    std::vector<MidiPort> res;
    try
    {
      RtMidiIn midiIn;
      for (unsigned int i = 0; i < midiIn.getPortCount(); ++i)
      {
        auto name{midiIn.getPortName(i)};
        // our own virtual output is a source like any other, and must not be listed as one
        if (name != virtualOutputName) res.push_back({name, name, name});
      }
    }
    catch (RtMidiError &error)
    {
      // No MIDI system at all - on Linux this is the routine "ALSA sequencer is not
      // available" case. Report no ports; it is not a reason to end the process.
      LOGINFO("[ERROR] Unable to enumerate MIDI inputs: '{}'", error.getMessage());
      res.clear();
    }
    return res;
  }

  std::vector<MidiPort> outputs() override
  {
    std::vector<MidiPort> res;
    try
    {
      RtMidiOut midiOut;
      for (unsigned int i = 0; i < midiOut.getPortCount(); ++i)
      {
        auto name{midiOut.getPortName(i)};
        if (name != virtualInputName) res.push_back({name, name, name});
      }
    }
    catch (RtMidiError &error)
    {
      LOGINFO("[ERROR] Unable to enumerate MIDI outputs: '{}'", error.getMessage());
      res.clear();
    }
    return res;
  }

  std::vector<std::string> openInputs(const std::vector<std::string> &ids) override
  {
    // Resetting the pointers left the vector full of empty slots, so a second call
    // walked a list of nothing and a reopen appended to the stale entries.
    midiIns.clear();

    std::vector<std::string> opened;
    for (const auto &id : ids)
    {
      try
      {
        auto midiIn{std::make_unique<RtMidiIn>()};
        // by name: the list leaves our own virtual port out, so its positions are not
        // rtmidi's
        const int port{portIndex(*midiIn, id)};
        if (port < 0) continue;
        LOGDETAIL("MIDI: opening '{}'", id);
        midiIn->openPort(port);
        // sysex too, as the other backends deliver it; timing and active sensing not
        midiIn->ignoreTypes(false, true, true);
        midiIn->setCallback(callback, this);
        midiIns.push_back(std::move(midiIn));
        opened.push_back(id);
      }
      catch (RtMidiError &error)
      {
        LOGINFO("[ERROR] Unable to open MIDI input '{}': '{}'", id, error.getMessage());
      }
    }
    return opened;
  }

  std::vector<std::string> openOutputs(const std::vector<std::string> &ids) override
  {
    midiOuts.clear();

    std::vector<std::string> opened;
    for (const auto &id : ids)
    {
      try
      {
        auto midiOut{std::make_unique<RtMidiOut>()};
        const int port{portIndex(*midiOut, id)};
        if (port < 0) continue;
        LOGDETAIL("MIDI: opening output '{}'", id);
        midiOut->openPort(port);
        midiOuts.push_back(std::move(midiOut));
        opened.push_back(id);
      }
      catch (RtMidiError &error)
      {
        LOGINFO("[ERROR] Unable to open MIDI output '{}': '{}'", id, error.getMessage());
      }
    }
    return opened;
  }

  bool supportsVirtualPorts() const override
  {
    // what the midi api can do; every macos version has coremidi's virtual endpoints
    std::vector<RtMidi::Api> apis;
    RtMidi::getCompiledApi(apis);
    return std::any_of(
        apis.begin(), apis.end(), [](auto api)
        { return api == RtMidi::MACOSX_CORE || api == RtMidi::LINUX_ALSA || api == RtMidi::UNIX_JACK; });
  }

  bool setVirtualInput(const std::string &name) override
  {
    virtualIn.reset();
    virtualInputName.clear();
    if (name.empty()) return true;

    try
    {
      auto midiIn{std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, name)};
      midiIn->openVirtualPort(name);
      midiIn->ignoreTypes(false, true, true);
      midiIn->setCallback(callback, this);
      virtualIn = std::move(midiIn);
      virtualInputName = name;
      return true;
    }
    catch (RtMidiError &error)
    {
      LOGINFO("[ERROR] Unable to create the virtual MIDI input '{}': '{}'", name, error.getMessage());
      return false;
    }
  }

  bool setVirtualOutput(const std::string &name) override
  {
    virtualOut.reset();
    virtualOutputName.clear();
    if (name.empty()) return true;

    try
    {
      auto midiOut{std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, name)};
      midiOut->openVirtualPort(name);
      virtualOut = std::move(midiOut);
      virtualOutputName = name;
      return true;
    }
    catch (RtMidiError &error)
    {
      LOGINFO("[ERROR] Unable to create the virtual MIDI output '{}': '{}'", name, error.getMessage());
      return false;
    }
  }

  void send(const uint8_t *data, size_t size) override
  {
    auto sendTo = [data, size](RtMidiOut &out)
    {
      try
      {
        out.sendMessage(data, size);
      }
      catch (RtMidiError &error)
      {
        LOGINFO("[ERROR] Unable to send MIDI: '{}'", error.getMessage());
      }
    };
    for (auto &out : midiOuts) sendTo(*out);
    if (virtualOut) sendTo(*virtualOut);
  }

 private:
  static void callback(double, std::vector<unsigned char> *message, void *userData)
  {
    auto *self{static_cast<RtMidiBackend *>(userData)};
    if (message && !message->empty() && self->onMessage)
      self->onMessage(message->data(), message->size());
  }

  std::vector<std::unique_ptr<RtMidiIn>> midiIns;
  std::vector<std::unique_ptr<RtMidiOut>> midiOuts;
  std::unique_ptr<RtMidiIn> virtualIn;
  std::unique_ptr<RtMidiOut> virtualOut;
  std::string virtualInputName, virtualOutputName;
};
}  // namespace

std::unique_ptr<MidiBackend> createRtMidiBackend()
{
  return std::make_unique<RtMidiBackend>();
}
}  // namespace freeaudio::clap_wrapper::standalone
