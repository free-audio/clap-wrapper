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
#include "RtMidi.h"

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
          "Window:\n"
          "  --no-gui                 Run without a window. Audio, MIDI and the plugin's\n"
          "                           own timers still run; end it with ^C.\n"
          "\n"
          "Information:\n"
          "  --list-apis              Audio backends this build has, and what each sees.\n"
          "  --list-devices           Audio devices for the chosen (or default) api.\n"
          "  --list-midi-inputs       MIDI input ports. All of them are bound.\n"
          "  --version\n"
          "  --help\n"
          "\n"
          "Device names are the stable way to name a device: the numeric ids are handles\n"
          "which can differ between runs. A name is matched exactly if it can be, and\n"
          "otherwise as a unique fragment, so --output-device HDMI is usually enough.\n",
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

void listMidiInputs()
{
  try
  {
    RtMidiIn midiIn;
    auto n = midiIn.getPortCount();
    fprintf(stdout, "MIDI input ports (all are bound):\n");
    if (n == 0) fprintf(stdout, "  (none)\n");
    for (unsigned int i = 0; i < n; ++i)
    {
      fprintf(stdout, "  [%u] %s\n", i, midiIn.getPortName(i).c_str());
    }
  }
  catch (RtMidiError &e)
  {
    fprintf(stderr, "[ERROR] Unable to enumerate MIDI inputs : %s\n", e.getMessage().c_str());
  }
}

/*
 * Resolve what the user typed against the devices the host's own RtAudio
 * instance can see. RtAudio 6 device ids are per-instance handles rather than
 * stable identifiers, which is why a name is the better thing to pass and why
 * this resolves at startup rather than storing an id.
 */
bool resolveDevice(const std::vector<RtAudio::DeviceInfo> &devices, const std::string &spec,
                   bool forInput, unsigned int &resolved)
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
        resolved = d.ID;
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
      resolved = d.ID;
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
    resolved = partial.front()->ID;
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
}  // namespace

bool CommandLineOptions::anyAudioOverride() const
{
  return noInput || !inputDevice.empty() || !outputDevice.empty() || sampleRate > 0 || bufferSize > 0;
}

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

  if (wantApis || wantDevices || wantMidi)
  {
    if (wantApis) listApis();
    if (wantApis && (wantDevices || wantMidi)) fprintf(stdout, "\n");
    if (wantDevices) listDevices(opts.audioApi);
    if (wantDevices && wantMidi) fprintf(stdout, "\n");
    if (wantMidi) listMidiInputs();
    return CommandLineResult::exitOk;
  }

  return CommandLineResult::run;
}

bool applyCommandLineOptions(const CommandLineOptions &opts)
{
  if (!opts.anyAudioOverride()) return true;

  auto host = getStandaloneHost();

  // Anything not named on the command line keeps the default it would have had
  auto [defaultIn, defaultOut, defaultRate] = host->getDefaultAudioInOutSampleRate();
  auto in = defaultIn;
  auto out = defaultOut;
  auto rate = (opts.sampleRate > 0) ? opts.sampleRate : defaultRate;

  if (opts.noInput)
  {
    // The shared layer reads a device of 0 as 'no input'
    in = 0;
  }
  else if (!opts.inputDevice.empty())
  {
    if (!resolveDevice(host->getInputAudioDevices(), opts.inputDevice, true, in)) return false;
  }

  if (!opts.outputDevice.empty())
  {
    if (!resolveDevice(host->getOutputAudioDevices(), opts.outputDevice, false, out)) return false;
  }

  if (opts.sampleRate > 0)
  {
    // startAudioThreadOn falls back to the device's preferred rate if this one
    // isn't offered, which is a silent substitution unless we say something
    try
    {
      auto info = host->rtaDac->getDeviceInfo(out);
      auto &rates = info.sampleRates;
      if (std::find(rates.begin(), rates.end(), (unsigned int)opts.sampleRate) == rates.end())
      {
        fprintf(stderr,
                "[WARNING] '%s' does not offer %d Hz (it offers %s); the device's own rate will "
                "be used instead\n",
                info.name.c_str(), opts.sampleRate, ratesToString(rates).c_str());
      }
    }
    catch (...)
    {
    }
  }

  host->setStartupAudio(in, out, rate);

  if (opts.bufferSize > 0)
  {
    constexpr int minBuffer{16}, maxBuffer{8192};
    auto frames = std::clamp(opts.bufferSize, minBuffer, maxBuffer);
    if (frames != opts.bufferSize)
    {
      fprintf(stderr, "[WARNING] Buffer size %d is outside %d-%d frames; using %d\n", opts.bufferSize,
              minBuffer, maxBuffer, frames);
    }
    // the shared layer treats this as the requested size and RtAudio writes
    // back what it actually got
    host->currentBufferSize = (uint32_t)frames;
  }

  return true;
}
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
