#pragma once

#include <string>

/*
 * The Linux standalone is configured from the command line rather than from a
 * settings window: building a device picker in raw Xlib is a lot of work for a
 * poor result, and a standalone on Linux is usually started from a shell or a
 * script anyway.
 *
 * The flags cover the whole of what the audio layer can be told:
 *
 *   --audio-api <name>       alsa | pulse | jack | pipewire (= pulse) | auto
 *   --input-device <spec>    device name (or part of one), or an id from
 *   --output-device <spec>   --list-devices
 *   --no-input               open output only, even for a plugin with an input
 *   --sample-rate <hz>
 *   --buffer-size <frames>
 *   --no-gui                 run without a window
 *   --list-apis              what this build can talk to, and what it can see
 *   --list-devices           audio devices for the chosen (or default) api
 *   --list-midi-inputs       MIDI input ports, all of which get bound
 *   --version
 *   --help
 */
namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
struct CommandLineOptions
{
  std::string audioApi;      // empty for the default preference order
  std::string inputDevice;   // name, part of a name, or an RtAudio device id
  std::string outputDevice;  // likewise
  bool noInput{false};
  int sampleRate{0};  // 0 for the device's own rate
  int bufferSize{0};  // 0 for the shared default
  bool noGui{false};

  // whether anything here needs to override the host's startup defaults
  bool anyAudioOverride() const;
};

enum class CommandLineResult
{
  run,        // nothing to do but start up
  exitOk,     // we printed what was asked for
  exitError,  // bad usage; the message is already on stderr
};

/*
 * Parse argv into opts. Handles --help and the --list- flags itself, in which
 * case the result is exitOk and main should return 0.
 */
CommandLineResult parseCommandLine(int argc, char **argv, const std::string &programName,
                                   CommandLineOptions &opts);

/*
 * Hand the audio choices to the standalone host. Must be called after the audio
 * api is selected and before audio starts. False means a device or rate the
 * user asked for doesn't exist, which is a startup error rather than something
 * to paper over with a default.
 */
bool applyCommandLineOptions(const CommandLineOptions &opts);
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
