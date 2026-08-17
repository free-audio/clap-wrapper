#include "linux_command_line.h"
#include "linux_frontend.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtAudio.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"
#include "detail/standalone/entry.h"

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
namespace
{
std::string lower(const std::string &s)
{
  std::string r{s};
  std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return r;
}

bool isAllDigits(const std::string &s)
{
  return !s.empty() &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

void usage(const std::string &programName)
{
  fprintf(stdout,
          "%s - a CLAP plugin as a standalone application.\n"
          "\n"
          "Usage: %s [options]\n"
          "\n"
          "Audio:\n"
          "  --audio-api <name>       alsa, pulse, jack, pipewire (an alias for pulse,\n"
          "                           which is how a PipeWire graph is reached), or auto.\n"
          "                           Default: the first of pulse, jack, alsa which has a\n"
          "                           device.\n"
          "  --output-device <spec>   Device name, part of a name, or an id from\n"
          "  --input-device <spec>    --list-devices. Default: the system default device.\n"
          "  --no-input               Open output only, even for a plugin with an audio\n"
          "                           input.\n"
          "  --sample-rate <hz>       Default: whatever the device is running at.\n"
          "  --buffer-size <frames>   Default: 256.\n"
          "\n"
          "MIDI:\n"
          "  --midi-input <spec>      Port name, part of a name, or an index from\n"
          "                           --list-midi-inputs. Repeat it for more than one\n"
          "                           port. Default: every port is bound.\n"
          "  --no-midi                Bind no MIDI input at all.\n"
          "\n"
          "Window:\n"
          "  --no-gui                 Run without a window. Audio, MIDI and the plugin's\n"
          "                           own timers still run; end it with ^C.\n"
          "\n"
          "Information:\n"
          "  --list-apis              Audio backends this build has, and what each sees.\n"
          "  --list-devices           Audio devices for the chosen (or default) api.\n"
          "  --list-midi-inputs       MIDI input ports, and which ones would be opened.\n"
          "  --version\n"
          "  --help\n"
          "\n"
          "Device and port names are the stable way to name one: the numeric ids are\n"
          "handles which can differ between runs. A name is matched exactly if it can be,\n"
          "and otherwise as a unique fragment, so --output-device HDMI is usually enough.\n"
          "\n"
          "These options override the persisted settings for this run only; they are not\n"
          "written back to the settings file.\n",
          programName.c_str(), programName.c_str());
}

void printVersion(const std::string &programName)
{
#ifdef CLAP_WRAPPER_VERSION
  fprintf(stdout, "%s - CLAP standalone, clap-wrapper %s\n", programName.c_str(), CLAP_WRAPPER_VERSION);
#else
  fprintf(stdout, "%s - CLAP standalone\n", programName.c_str());
#endif
}

std::string ratesToString(const std::vector<unsigned int> &rates)
{
  std::string res;
  for (auto r : rates)
  {
    if (!res.empty()) res += " ";
    res += std::to_string(r);
  }
  return res.empty() ? std::string("(none reported)") : res;
}

void listApis()
{
  fprintf(stdout, "Audio APIs in this build:\n");
  for (auto api : compiledAudioApis())
  {
    if (api == RtAudio::Api::RTAUDIO_DUMMY) continue;

    // Count what each one can see: a backend with no devices is one whose
    // server isn't running, which is worth saying out loud
    unsigned int outs{0}, ins{0};
    try
    {
      RtAudio probe(api, [](RtAudioErrorType, const std::string &) {});
      for (auto id : probe.getDeviceIds())
      {
        auto info = probe.getDeviceInfo(id);
        if (info.outputChannels > 0) outs++;
        if (info.inputChannels > 0) ins++;
      }
    }
    catch (...)
    {
    }

    fprintf(stdout, "  %-8s %-12s %u output, %u input device(s)%s\n", RtAudio::getApiName(api).c_str(),
            RtAudio::getApiDisplayName(api).c_str(), outs, ins,
            (outs == 0 && ins == 0) ? "  (nothing there - is the server running?)" : "");
  }
}

void listDevices(const std::string &requestedApi)
{
  auto api = resolveAudioApiName(requestedApi);

  try
  {
    RtAudio rta(api, [](RtAudioErrorType, const std::string &msg)
                { fprintf(stderr, "[ERROR] %s\n", msg.c_str()); });

    fprintf(stdout, "Audio api: %s (%s)\n\n", RtAudio::getApiDisplayName(rta.getCurrentApi()).c_str(),
            RtAudio::getApiName(rta.getCurrentApi()).c_str());

    auto ids = rta.getDeviceIds();
    if (ids.empty())
    {
      fprintf(stdout, "No devices.\n");
      return;
    }

    for (auto forInput : {false, true})
    {
      fprintf(stdout, "%s devices:\n", forInput ? "Input" : "Output");
      bool any{false};
      for (auto id : ids)
      {
        auto info = rta.getDeviceInfo(id);
        auto channels = forInput ? info.inputChannels : info.outputChannels;
        if (channels == 0) continue;
        any = true;

        auto isDefault = forInput ? info.isDefaultInput : info.isDefaultOutput;
        fprintf(stdout, "  [%u] %s%s\n", info.ID, info.name.c_str(), isDefault ? "  (default)" : "");
        fprintf(stdout, "      %u channel(s), current rate %u, rates: %s\n", channels,
                info.currentSampleRate, ratesToString(info.sampleRates).c_str());
      }
      if (!any) fprintf(stdout, "  (none)\n");
    }
  }
  catch (const std::exception &e)
  {
    fprintf(stderr, "[ERROR] Unable to enumerate audio devices : %s\n", e.what());
  }
}

bool resolveMidiPort(const std::vector<std::string> &ports, const std::string &spec,
                     std::string &resolved);

void listMidiInputs(const CommandLineOptions &opts)
{
  auto ports = getStandaloneHost()->getMidiPortNames();

  if (opts.noMidi)
    fprintf(stdout, "MIDI input ports (--no-midi, so none is opened):\n");
  else if (opts.midiInputs.empty())
    fprintf(stdout, "MIDI input ports (all are opened):\n");
  else
    fprintf(stdout, "MIDI input ports ('*' marks the ones --midi-input picks):\n");

  if (ports.empty()) fprintf(stdout, "  (none)\n");

  // Resolve the selection here too, so --list-midi-inputs is also how you check
  // that what you are about to pass actually matches something
  std::vector<std::string> selected;
  for (const auto &spec : opts.midiInputs)
  {
    std::string name;
    if (resolveMidiPort(ports, spec, name)) selected.push_back(name);
  }

  for (unsigned int i = 0; i < ports.size(); ++i)
  {
    auto picked = std::find(selected.begin(), selected.end(), ports[i]) != selected.end();
    fprintf(stdout, "  %s [%u] %s\n", picked ? "*" : " ", i, ports[i].c_str());
  }
}

/*
 * Resolve what the user typed against the devices the host's own RtAudio
 * instance can see, and yield the device's *name*: that is what the settings
 * layer stores and matches on, because RtAudio 6 device ids are per-instance
 * enumeration handles rather than stable identifiers. Ids are still accepted as
 * input, since --list-devices prints them, but they are never carried further
 * than this function.
 */
bool resolveDevice(const std::vector<RtAudio::DeviceInfo> &devices, const std::string &spec,
                   bool forInput, std::string &resolved)
{
  auto what = forInput ? "input" : "output";

  auto complain = [&](const std::string &why)
  {
    fprintf(stderr, "[ERROR] %s audio device '%s': %s\n", what, spec.c_str(), why.c_str());
    fprintf(stderr, "        Available %s devices:\n", what);
    for (const auto &d : devices) fprintf(stderr, "          [%u] %s\n", d.ID, d.name.c_str());
  };

  if (isAllDigits(spec))
  {
    auto asId = (unsigned int)strtoul(spec.c_str(), nullptr, 10);
    for (const auto &d : devices)
    {
      if (d.ID == asId)
      {
        resolved = d.name;
        return true;
      }
    }
    complain("no device with that id");
    return false;
  }

  auto needle = lower(spec);

  for (const auto &d : devices)
  {
    if (lower(d.name) == needle)
    {
      resolved = d.name;
      return true;
    }
  }

  std::vector<const RtAudio::DeviceInfo *> partial;
  for (const auto &d : devices)
  {
    if (lower(d.name).find(needle) != std::string::npos) partial.push_back(&d);
  }

  if (partial.size() == 1)
  {
    resolved = partial.front()->name;
    return true;
  }
  if (partial.empty())
  {
    complain("no such device");
    return false;
  }

  std::string matches;
  for (auto *d : partial)
  {
    if (!matches.empty()) matches += ", ";
    matches += "'" + d->name + "'";
  }
  complain("matches more than one device: " + matches);
  return false;
}

/*
 * The same resolution for MIDI, against the port names RtMidi reports. The index
 * accepted here is the position in that list - which is what --list-midi-inputs
 * prints - and is no more stable across a reboot than an audio device id is.
 */
bool resolveMidiPort(const std::vector<std::string> &ports, const std::string &spec,
                     std::string &resolved)
{
  auto complain = [&](const std::string &why)
  {
    fprintf(stderr, "[ERROR] MIDI input '%s': %s\n", spec.c_str(), why.c_str());
    fprintf(stderr, "        Available MIDI inputs:\n");
    if (ports.empty()) fprintf(stderr, "          (none)\n");
    for (unsigned int i = 0; i < ports.size(); ++i)
    {
      fprintf(stderr, "          [%u] %s\n", i, ports[i].c_str());
    }
  };

  if (isAllDigits(spec))
  {
    auto idx = strtoul(spec.c_str(), nullptr, 10);
    if (idx < ports.size())
    {
      resolved = ports[idx];
      return true;
    }
    complain("no port with that index");
    return false;
  }

  auto needle = lower(spec);

  for (const auto &port : ports)
  {
    if (lower(port) == needle)
    {
      resolved = port;
      return true;
    }
  }

  std::vector<const std::string *> partial;
  for (const auto &port : ports)
  {
    if (lower(port).find(needle) != std::string::npos) partial.push_back(&port);
  }

  if (partial.size() == 1)
  {
    resolved = *partial.front();
    return true;
  }
  if (partial.empty())
  {
    complain("no such port");
    return false;
  }

  std::string matches;
  for (auto *port : partial)
  {
    if (!matches.empty()) matches += ", ";
    matches += "'" + *port + "'";
  }
  complain("matches more than one port: " + matches);
  return false;
}

/*
 * Layer the command line over whatever the settings file said. Everything lands
 * in host->settings rather than in the host's live audio fields, so that the
 * shared applyAudioSettings() does the API-then-device ordering exactly once and
 * we are not maintaining a second copy of it here.
 */
bool overlayCommandLine(const CommandLineOptions &opts)
{
  auto host = getStandaloneHost();
  auto &settings = host->settings;

  if (opts.noInput)
  {
    // Leave the device name alone: not using the input is a different thing from
    // forgetting which input was configured.
    settings.audioInputUsed = false;
  }
  else if (!opts.inputDevice.empty())
  {
    if (!resolveDevice(host->getInputAudioDevices(), opts.inputDevice, true, settings.inputDeviceName))
    {
      return false;
    }
    settings.audioInputUsed = true;
  }

  if (!opts.outputDevice.empty())
  {
    if (!resolveDevice(host->getOutputAudioDevices(), opts.outputDevice, false,
                       settings.outputDeviceName))
    {
      return false;
    }
    settings.audioOutputUsed = true;
  }

  if (opts.sampleRate > 0)
  {
    settings.sampleRate = opts.sampleRate;
  }

  if (opts.bufferSize > 0)
  {
    constexpr int minBuffer{16}, maxBuffer{8192};
    auto frames = std::clamp(opts.bufferSize, minBuffer, maxBuffer);
    if (frames != opts.bufferSize)
    {
      fprintf(stderr, "[WARNING] Buffer size %d is outside %d-%d frames; using %d\n", opts.bufferSize,
              minBuffer, maxBuffer, frames);
    }
    // The requested size. RtAudio writes back what it actually granted.
    settings.bufferSize = (uint32_t)frames;
  }

  if (opts.noMidi)
  {
    // An empty list with bindAll off is the deliberate "no MIDI input" case
    settings.midiBindAllPorts = false;
    settings.midiPortNames.clear();
  }
  else if (!opts.midiInputs.empty())
  {
    auto ports = host->getMidiPortNames();
    std::vector<std::string> names;
    for (const auto &spec : opts.midiInputs)
    {
      std::string name;
      if (!resolveMidiPort(ports, spec, name)) return false;
      if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    settings.midiBindAllPorts = false;
    settings.midiPortNames = names;
  }

  return true;
}

/*
 * startAudioThreadOnImpl silently substitutes the device's preferred rate for one
 * it doesn't offer. That is the right thing to do, but not silently when the user
 * named the rate on the command line.
 */
void warnIfRateSubstituted(int requestedRate)
{
  auto host = getStandaloneHost();
  if (!host->audioOutputUsed || !host->isKnownDevice(host->audioOutputDeviceID)) return;

  auto info = host->deviceInfoFor(host->audioOutputDeviceID);
  const auto &rates = info.sampleRates;
  if (std::find(rates.begin(), rates.end(), (unsigned int)requestedRate) != rates.end()) return;

  fprintf(stderr,
          "[WARNING] '%s' does not offer %d Hz (it offers %s); the device's own rate will be "
          "used instead\n",
          info.name.c_str(), requestedRate, ratesToString(rates).c_str());
}
}  // namespace

CommandLineResult parseCommandLine(int argc, char **argv, const std::string &programName,
                                   CommandLineOptions &opts)
{
  bool wantApis{false}, wantDevices{false}, wantMidi{false};

  // --opt value and --opt=value both work; a missing value is an error rather
  // than a silently empty string
  auto valueFor = [&](int &i, const std::string &arg, const std::string &name, std::string &into) -> bool
  {
    auto eq = arg.find('=');
    if (eq != std::string::npos)
    {
      into = arg.substr(eq + 1);
    }
    else if (i + 1 < argc)
    {
      into = argv[++i];
    }
    else
    {
      fprintf(stderr, "[ERROR] %s needs a value\n", name.c_str());
      return false;
    }

    if (into.empty())
    {
      fprintf(stderr, "[ERROR] %s needs a value\n", name.c_str());
      return false;
    }
    return true;
  };

  auto intValueFor = [&](int &i, const std::string &arg, const std::string &name, int &into) -> bool
  {
    std::string s;
    if (!valueFor(i, arg, name, s)) return false;
    if (!isAllDigits(s))
    {
      fprintf(stderr, "[ERROR] %s wants a number, not '%s'\n", name.c_str(), s.c_str());
      return false;
    }
    into = std::atoi(s.c_str());
    return true;
  };

  for (int i = 1; i < argc; ++i)
  {
    std::string arg{argv[i] ? argv[i] : ""};
    auto name = arg.substr(0, arg.find('='));

    if (arg == "-h" || arg == "--help")
    {
      usage(programName);
      return CommandLineResult::exitOk;
    }
    else if (arg == "--version")
    {
      printVersion(programName);
      return CommandLineResult::exitOk;
    }
    else if (arg == "--list-apis")
    {
      wantApis = true;
    }
    else if (arg == "--list-devices")
    {
      wantDevices = true;
    }
    else if (arg == "--list-midi-inputs")
    {
      wantMidi = true;
    }
    else if (arg == "--no-input")
    {
      opts.noInput = true;
    }
    else if (arg == "--no-midi")
    {
      opts.noMidi = true;
    }
    else if (arg == "--no-gui")
    {
      opts.noGui = true;
    }
    else if (name == "--audio-api")
    {
      if (!valueFor(i, arg, name, opts.audioApi)) return CommandLineResult::exitError;
    }
    else if (name == "--input-device")
    {
      if (!valueFor(i, arg, name, opts.inputDevice)) return CommandLineResult::exitError;
    }
    else if (name == "--output-device")
    {
      if (!valueFor(i, arg, name, opts.outputDevice)) return CommandLineResult::exitError;
    }
    else if (name == "--midi-input")
    {
      // Repeatable, so this appends rather than assigns
      std::string spec;
      if (!valueFor(i, arg, name, spec)) return CommandLineResult::exitError;
      opts.midiInputs.push_back(spec);
    }
    else if (name == "--sample-rate")
    {
      if (!intValueFor(i, arg, name, opts.sampleRate)) return CommandLineResult::exitError;
    }
    else if (name == "--buffer-size")
    {
      if (!intValueFor(i, arg, name, opts.bufferSize)) return CommandLineResult::exitError;
    }
    else if (arg.rfind("-", 0) == 0)
    {
      fprintf(stderr, "[ERROR] Unknown option '%s'. Try --help.\n", arg.c_str());
      return CommandLineResult::exitError;
    }
    else
    {
      // Not an option. A launcher may well have appended something; say so and
      // carry on rather than refusing to start.
      fprintf(stderr, "[WARNING] Ignoring argument '%s'\n", arg.c_str());
    }
  }

  if (!opts.audioApi.empty() && lower(opts.audioApi) != "auto" && lower(opts.audioApi) != "default" &&
      resolveAudioApiName(opts.audioApi) == RtAudio::Api::UNSPECIFIED)
  {
    std::string available;
    for (auto a : compiledAudioApis())
    {
      if (a == RtAudio::Api::RTAUDIO_DUMMY) continue;
      if (!available.empty()) available += ", ";
      available += RtAudio::getApiName(a);
    }
    fprintf(stderr, "[ERROR] No audio api called '%s' in this build. Available: %s\n",
            opts.audioApi.c_str(), available.c_str());
    return CommandLineResult::exitError;
  }

  if (opts.noInput && !opts.inputDevice.empty())
  {
    fprintf(stderr, "[ERROR] --no-input and --input-device contradict each other\n");
    return CommandLineResult::exitError;
  }

  if (opts.noMidi && !opts.midiInputs.empty())
  {
    fprintf(stderr, "[ERROR] --no-midi and --midi-input contradict each other\n");
    return CommandLineResult::exitError;
  }

  if (wantApis || wantDevices || wantMidi)
  {
    if (wantApis) listApis();
    if (wantApis && (wantDevices || wantMidi)) fprintf(stdout, "\n");
    if (wantDevices) listDevices(opts.audioApi);
    if (wantDevices && wantMidi) fprintf(stdout, "\n");
    if (wantMidi) listMidiInputs(opts);
    return CommandLineResult::exitOk;
  }

  return CommandLineResult::run;
}

bool configureAndStartAudio(const CommandLineOptions &opts)
{
  auto host = getStandaloneHost();

  // The persisted settings are the baseline the flags override. This is the load
  // startAudioThread() would do for itself; we do it here because the overrides
  // have to go on top of it, and a second load down there would undo them.
  host->loadStandaloneSettings();

  // The backend has to be chosen before any device is named: a device name is
  // resolved against one particular backend's enumeration.
  selectAudioApi(opts.audioApi);

  if (!overlayCommandLine(opts)) return false;

  try
  {
    host->applyAudioSettings();
  }
  catch (const std::exception &e)
  {
    // Enumeration itself throws when there is nothing usable attached. That is
    // not a startup failure: the plugin, its GUI and MIDI all still work, so say
    // so and carry on without audio.
    reportError("Unable to configure audio", e.what());
    return true;
  }

  if (opts.sampleRate > 0) warnIfRateSubstituted(opts.sampleRate);

  // A rate of zero means "whatever the device is running at", so ask for that
  // rather than leaving it to startAudioThreadOnImpl, which substitutes the
  // device's *preferred* rate - not the same number on a Pulse or PipeWire graph
  // which has been moved off its default.
  if (host->currentSampleRate <= 0 && host->audioOutputUsed &&
      host->isKnownDevice(host->audioOutputDeviceID))
  {
    auto info = host->deviceInfoFor(host->audioOutputDeviceID);
    auto rate = info.currentSampleRate ? info.currentSampleRate : info.preferredSampleRate;
    host->currentSampleRate = (int32_t)rate;
  }

  host->startMIDIThread();
  host->startAudioThreadOn(host->audioInputDeviceID, host->deviceInputChannels,
                           host->audioInputUsed && host->numAudioInputs > 0, host->audioOutputDeviceID,
                           host->deviceOutputChannels,
                           host->audioOutputUsed && host->numAudioOutputs > 0, host->currentSampleRate);

  return true;
}
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
