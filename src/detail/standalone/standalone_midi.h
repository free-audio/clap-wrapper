#pragma once

/*
 * The standalone's MIDI, with no platform api in it. One backend per build: CoreMIDI on
 * macOS, WinMM on Windows, or RtMidi's api with CLAP_WRAPPER_STANDALONE_RTAUDIO (always
 * on Linux).
 *
 * Nothing in here throws.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace freeaudio::clap_wrapper::standalone
{
struct MidiPort
{
  // what the settings and the standalone features extension keep. stable across launches
  // where the platform has such an id: CoreMIDI's unique id. WinMM has none, so there it
  // is the device's name and its place among ports of that name. RtMidi uses its name
  std::string id;

  // for a list; unique within its direction
  std::string name;

  // what RtMidi called this port, so that settings saved by an RtMidi build still find
  // it. empty where RtMidi would not have listed it
  std::string legacyName;
};

// the port a saved or requested reference names: by id, then by RtMidi's name, then by
// display name. null if none
const MidiPort *findMidiPort(const std::vector<MidiPort> &ports, const std::string &reference);

class MidiBackend
{
 public:
  virtual ~MidiBackend() = default;

  // "coremidi", "winmm", or RtMidi's api name ("core", "winmm", "alsa", "jack")
  virtual std::string apiId() const = 0;

  // [main-thread] the ports there are now. our own virtual ports are not among them, so
  // that binding every input can never loop the plugin's output back into it
  virtual std::vector<MidiPort> inputs() = 0;
  virtual std::vector<MidiPort> outputs() = 0;

  // [main-thread] opens exactly these ports (MidiPort::id) and closes every other; an id
  // that is not there is skipped. returns the ids that opened
  virtual std::vector<std::string> openInputs(const std::vector<std::string> &ids) = 0;
  virtual std::vector<std::string> openOutputs(const std::vector<std::string> &ids) = 0;

  // whether this api has ports other applications connect to (CoreMIDI, ALSA, JACK; not
  // WinMM)
  virtual bool supportsVirtualPorts() const = 0;

  // [main-thread] creates the virtual destination other applications send to under this
  // name, or removes it for an empty one. false if it could not be created
  virtual bool setVirtualInput(const std::string &name) = 0;
  // likewise for the virtual source they receive from
  virtual bool setVirtualOutput(const std::string &name) = 0;

  // [sender-thread] one complete message - a short message, or sysex from F0 to F7 - to
  // every open output and the virtual one. called from one thread at a time, never the
  // audio thread
  virtual void send(const uint8_t *data, size_t size) = 0;

  // [backend-threads] set before anything is opened. one complete message from an open
  // input or the virtual input, with its status byte (no running status); realtime bytes
  // that arrive inside a sysex come as messages of their own. may be called from several
  // threads at once
  std::function<void(const uint8_t *data, size_t size)> onMessage;

  // [any-thread] a port came or went; inputs() and outputs() list them again. the
  // handler hops to the main thread itself
  std::function<void()> onPortsChanged;
};

// this build's backend. null only for a build with no MIDI api
std::unique_ptr<MidiBackend> createMidiBackend();

}  // namespace freeaudio::clap_wrapper::standalone
