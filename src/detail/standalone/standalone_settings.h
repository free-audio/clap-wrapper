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
 * Devices and MIDI ports are identified by *name*, never by RtAudio's numeric
 * device id. Those ids are per-RtAudio-instance enumeration handles: they are not
 * stable across a restart, and not even across an API switch within one session.
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

  // The frame count to ask for when nothing has been chosen yet. RtAudio cannot be
  // asked what a device supports; you find out by trying to open it. 256 is the
  // value that works essentially everywhere.
  static constexpr uint32_t defaultBufferSize{256};

  int version{currentVersion};

  std::string audioApiName;  // RtAudio::getApiName(); empty means unspecified

  // The devices the user *asked for*, and whether they want that side of the
  // stream at all. These are written only when the user makes a choice, never
  // from the device the standalone actually ended up opening: a saved device
  // that is unplugged today resolves to the default for this session and is
  // still the saved device tomorrow, and a session on a machine with no
  // playback endpoint (an RDP session without audio redirection) leaves
  // audioOutputUsed alone rather than switching output off for good.
  std::string inputDeviceName;   // empty means the system default device
  std::string outputDeviceName;  // empty means the system default device
  bool audioInputUsed{true};     // false only when the user muted input
  bool audioOutputUsed{true};    // no UI turns this off yet; the .conf can

  int32_t sampleRate{0};  // 0 means the device's preferred rate
  // Clamped on apply to the largest size the settings panel offers; see
  // StandaloneHost::applyAudioSettings() for why a .conf value cannot be trusted.
  uint32_t bufferSize{defaultBufferSize};

  // An empty selection means "open every port", which is what the standalone did
  // before ports could be selected at all, and is the friendlier first run.
  bool midiBindAllPorts{true};
  std::vector<std::string> midiPortNames;

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
