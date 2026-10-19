#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "detail/clap/fsutil.h"

namespace freeaudio::clap_wrapper::standalone
{
/*
 * The standalone's audio and MIDI configuration, persisted alongside the plugin
 * state blob. This is the "settings envelope" the state streaming code has always
 * wanted: plugin state answers what the plugin sounds like, this answers what it
 * is plugged into.
 *
 * Devices and MIDI ports are identified by the backend's stable id (AudioDevice::id,
 * MidiPort::id) and by name. The name is the one an RtAudio or RtMidi build gave them,
 * where there is one, so that a build from before the native backends still finds them;
 * an id whose device now has another name was rewritten by such a build, and the name
 * wins (StandaloneHost::resolveInputDevice). Never by a numeric enumeration index,
 * which is not stable across a restart or an API switch.
 *
 * The on-disk format is flat UTF-8 key=value text, one pair per line, '#' comments,
 * so that a platform with no settings UI still has a supported way to configure the
 * standalone. Keys we do not recognise survive a load/save round trip untouched, so
 * settings written by a newer build are not silently destroyed by an older one.
 *
 * Leading and trailing whitespace is trimmed from both key and value, so a value
 * cannot begin or end with a space. No device or port name in practice does.
 */
struct StandaloneSettings
{
  static constexpr int currentVersion{1};

  // The frame count to ask for when nothing has been chosen yet. 256 is the value that
  // works essentially everywhere.
  static constexpr uint32_t defaultBufferSize{256};

  int version{currentVersion};

  std::string audioApiName;  // AudioApi::id, RtAudio's api name; empty means the default

  // What the user asked for, written only at the point of choice - never from the
  // device actually opened or the rate actually granted, which may be a fallback.
  // Empty means the system default device.
  std::string inputDeviceId, outputDeviceId;
  std::string inputDeviceName, outputDeviceName;
  bool audioInputUsed{true};
  bool audioOutputUsed{true};  // no UI turns this off yet; the .conf can
  int32_t sampleRate{0};       // 0 means the device's preferred rate

  // the device channel, 1-based, the plugin's main input is fed from and its main output
  // sent to; 0 means the device's first (StandaloneHost::channelChoiceFor)
  uint32_t inputFirstChannel{0}, outputFirstChannel{0};

  // Clamped on apply; see StandaloneHost::applyAudioSettings().
  uint32_t bufferSize{defaultBufferSize};

  // An empty selection means "open every port", which is what the standalone did
  // before ports could be selected at all, and is the friendlier first run.
  // The names are what a build from before the ids reads. Next to them, each port's id
  // with the name it was saved under: an older build rewrites the names and keeps these
  // as keys it does not know, so they count only while they name what the names do
  // (StandaloneHost::savedMidiSelection).
  struct SavedPort
  {
    std::string id, name;
  };
  bool midiBindAllPorts{true};
  std::vector<std::string> midiPortNames;
  std::vector<SavedPort> midiPortIds;

  // The MIDI outputs. Empty opens none, and there is no "open every port": that would
  // play the plugin's output into everything plugged in.
  std::vector<std::string> midiOutputPortNames;
  std::vector<SavedPort> midiOutputPortIds;

  // Virtual ports other applications connect to, where the MIDI API has them (CoreMIDI,
  // ALSA, JACK - not Windows). Off unless asked for.
  bool midiVirtualInput{false};
  bool midiVirtualOutput{false};

  // the settings screen's appearance as the screen's own menu chose it, a
  // clap_wrapper_standalone_appearance: 0 auto, 1 light, 2 dark. a plugin that sets the
  // appearance itself is never saved here
  uint32_t screenAppearance{0};

  bool hasWindowPosition{false};
  int32_t windowX{0}, windowY{0};
  uint32_t windowWidth{0}, windowHeight{0};

  // Keys from a future version, carried through verbatim on rewrite.
  std::vector<std::pair<std::string, std::string>> unknownKeys;

  // Neither of these throws; both report failure by returning false.
  bool load(const fs::path &fromFile);
  bool save(const fs::path &intoFile) const;
};
}  // namespace freeaudio::clap_wrapper::standalone
