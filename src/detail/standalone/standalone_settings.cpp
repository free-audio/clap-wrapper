#include "standalone_settings.h"
#include "standalone_details.h"

#include <cstdlib>
#include <fstream>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
std::string trim(const std::string &in)
{
  auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };

  size_t b{0}, e{in.size()};
  while (b < e && isSpace(static_cast<unsigned char>(in[b]))) ++b;
  while (e > b && isSpace(static_cast<unsigned char>(in[e - 1]))) --e;

  return in.substr(b, e - b);
}

// Only the characters which would break the line-oriented format need escaping.
// Everything else, UTF-8 included, is written through as-is.
std::string escape(const std::string &in)
{
  std::string out;
  out.reserve(in.size());

  for (auto c : in)
  {
    switch (c)
    {
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        out += c;
        break;
    }
  }

  return out;
}

std::string unescape(const std::string &in)
{
  std::string out;
  out.reserve(in.size());

  for (size_t i = 0; i < in.size(); ++i)
  {
    if (in[i] != '\\' || i + 1 == in.size())
    {
      out += in[i];
      continue;
    }

    switch (in[++i])
    {
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case '\\':
        out += '\\';
        break;
      default:
        // An escape we don't know: keep both characters rather than eat one.
        out += '\\';
        out += in[i];
        break;
    }
  }

  return out;
}

bool asBool(const std::string &v)
{
  return v == "true" || v == "1" || v == "yes";
}

const char *fromBool(bool v)
{
  return v ? "true" : "false";
}
}  // namespace

bool StandaloneSettings::load(const fs::path &fromFile)
{
  try
  {
    std::ifstream ifs(fromFile, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) return false;

    StandaloneSettings loaded;

    // Distinguish "this file predates MIDI selection" from "the user deselected
    // every port". Only an explicit key means the latter.
    bool sawMidiSelection{false};
    bool sawAnyKey{false};

    std::string line;
    while (std::getline(ifs, line))
    {
      auto trimmed = trim(line);
      if (trimmed.empty() || trimmed[0] == '#') continue;

      auto eq = trimmed.find('=');
      if (eq == std::string::npos) continue;

      // Split on the first '=' only: device names are allowed to contain one.
      auto key = trim(trimmed.substr(0, eq));
      auto value = unescape(trim(trimmed.substr(eq + 1)));
      if (key.empty()) continue;

      sawAnyKey = true;

      if (key == "version")
        loaded.version = std::atoi(value.c_str());
      else if (key == "audioApiName")
        loaded.audioApiName = value;
      else if (key == "inputDeviceName")
        loaded.inputDeviceName = value;
      else if (key == "outputDeviceName")
        loaded.outputDeviceName = value;
      else if (key == "audioInputUsed")
        loaded.audioInputUsed = asBool(value);
      else if (key == "audioOutputUsed")
        loaded.audioOutputUsed = asBool(value);
      else if (key == "sampleRate")
        loaded.sampleRate = static_cast<int32_t>(std::atol(value.c_str()));
      else if (key == "bufferSize")
        loaded.bufferSize = static_cast<uint32_t>(std::atol(value.c_str()));
      else if (key == "midiBindAllPorts")
      {
        loaded.midiBindAllPorts = asBool(value);
        sawMidiSelection = true;
      }
      else if (key == "midiPort")
      {
        loaded.midiPortNames.push_back(value);
        sawMidiSelection = true;
      }
      else if (key == "windowX")
      {
        loaded.windowX = static_cast<int32_t>(std::atol(value.c_str()));
        loaded.hasWindowPosition = true;
      }
      else if (key == "windowY")
        loaded.windowY = static_cast<int32_t>(std::atol(value.c_str()));
      else if (key == "windowWidth")
        loaded.windowWidth = static_cast<uint32_t>(std::atol(value.c_str()));
      else if (key == "windowHeight")
        loaded.windowHeight = static_cast<uint32_t>(std::atol(value.c_str()));
      else
        loaded.unknownKeys.emplace_back(key, value);
    }

    if (!sawAnyKey) return false;

    if (loaded.version > currentVersion)
    {
      LOGINFO(
          "[WARNING] Standalone settings are version {} but this build understands {}; "
          "reading what we recognise",
          loaded.version, currentVersion);
    }

    if (!sawMidiSelection)
    {
      // Written before port selection existed: keep binding everything.
      loaded.midiBindAllPorts = true;
      loaded.midiPortNames.clear();
    }

    *this = loaded;

    return true;
  }
  catch (const std::exception &e)
  {
    LOGINFO("[ERROR] Unable to read standalone settings: '{}'", e.what());
    return false;
  }
  catch (...)
  {
    LOGINFO("[ERROR] Unable to read standalone settings");
    return false;
  }
}

bool StandaloneSettings::save(const fs::path &intoFile) const
{
  try
  {
    std::ofstream ofs(intoFile, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!ofs.is_open())
    {
      LOGINFO("[ERROR] Unable to open standalone settings for writing '{}'", intoFile.u8string());
      return false;
    }

    ofs << "# clap-wrapper standalone settings.\n";
    ofs << "# Audio devices and MIDI ports are matched by name when the standalone starts.\n";
    ofs << "# An unmatched name falls back to the system default for that run, and is kept.\n";
    ofs << "version=" << currentVersion << "\n";

    ofs << "audioApiName=" << escape(audioApiName) << "\n";
    ofs << "outputDeviceName=" << escape(outputDeviceName) << "\n";
    ofs << "inputDeviceName=" << escape(inputDeviceName) << "\n";
    ofs << "audioOutputUsed=" << fromBool(audioOutputUsed) << "\n";
    ofs << "audioInputUsed=" << fromBool(audioInputUsed) << "\n";
    ofs << "sampleRate=" << sampleRate << "\n";
    ofs << "bufferSize=" << bufferSize << "\n";

    ofs << "midiBindAllPorts=" << fromBool(midiBindAllPorts) << "\n";
    for (const auto &port : midiPortNames)
    {
      ofs << "midiPort=" << escape(port) << "\n";
    }

    if (hasWindowPosition)
    {
      ofs << "windowX=" << windowX << "\n";
      ofs << "windowY=" << windowY << "\n";
      ofs << "windowWidth=" << windowWidth << "\n";
      ofs << "windowHeight=" << windowHeight << "\n";
    }

    for (const auto &[key, value] : unknownKeys)
    {
      ofs << escape(key) << "=" << escape(value) << "\n";
    }

    ofs.flush();

    return static_cast<bool>(ofs);
  }
  catch (const std::exception &e)
  {
    LOGINFO("[ERROR] Unable to write standalone settings: '{}'", e.what());
    return false;
  }
  catch (...)
  {
    LOGINFO("[ERROR] Unable to write standalone settings");
    return false;
  }
}
}  // namespace freeaudio::clap_wrapper::standalone
