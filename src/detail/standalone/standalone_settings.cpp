#include "standalone_settings.h"
#include "standalone_details.h"

#include <cstdlib>
#include <cstring>
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

// One `key=value` line, trimmed and unescaped. False for blank lines, comments and
// anything without a key.
bool splitLine(const std::string &line, std::string &key, std::string &value)
{
  auto trimmed = trim(line);
  if (trimmed.empty() || trimmed[0] == '#') return false;

  auto eq = trimmed.find('=');
  if (eq == std::string::npos) return false;

  // Split on the first '=' only: device names are allowed to contain one.
  key = trim(trimmed.substr(0, eq));
  value = unescape(trim(trimmed.substr(eq + 1)));
  return !key.empty();
}

const char *const windowFields[] = {"windowX", "windowY", "windowWidth", "windowHeight"};

// "windowX" for the first instance, "windowX.2" for the third.
std::string windowKey(const char *field, uint32_t instance)
{
  if (instance == 0) return field;
  return std::string(field) + "." + std::to_string(instance);
}

// Whether `key` is one of the window fields, and if so which field and instance.
bool isWindowKey(const std::string &key, std::string &field, uint32_t &instance)
{
  for (auto *name : windowFields)
  {
    const auto length{std::strlen(name)};
    if (key.compare(0, length, name) != 0) continue;

    if (key.size() == length)
    {
      field = name;
      instance = 0;
      return true;
    }

    // Only a dot and digits may follow, or "windowXYZ" would pass for windowX.
    if (key[length] != '.' || key.size() == length + 1) continue;
    if (key.find_first_not_of("0123456789", length + 1) != std::string::npos) continue;

    field = name;
    instance = static_cast<uint32_t>(std::strtoul(key.c_str() + length + 1, nullptr, 10));
    return true;
  }

  return false;
}
}  // namespace

bool StandaloneSettings::load(const fs::path &fromFile, uint32_t instance)
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

    std::string line, key, value, windowField;
    uint32_t windowInstance{0};
    while (std::getline(ifs, line))
    {
      if (!splitLine(line, key, value)) continue;

      sawAnyKey = true;

      if (isWindowKey(key, windowField, windowInstance))
      {
        // Another instance's window is that instance's business; save() carries it
        // over from the file as it is then.
        if (windowInstance != instance) continue;

        if (windowField == "windowX")
        {
          loaded.windowX = static_cast<int32_t>(std::atol(value.c_str()));
          loaded.hasWindowPosition = true;
        }
        else if (windowField == "windowY")
          loaded.windowY = static_cast<int32_t>(std::atol(value.c_str()));
        else if (windowField == "windowWidth")
          loaded.windowWidth = static_cast<uint32_t>(std::atol(value.c_str()));
        else if (windowField == "windowHeight")
          loaded.windowHeight = static_cast<uint32_t>(std::atol(value.c_str()));
        continue;
      }

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

bool StandaloneSettings::save(const fs::path &intoFile, uint32_t instance) const
{
  try
  {
    // The other instances' windows, as the file has them now. Read before the
    // truncating open below, which would leave nothing to read.
    std::vector<std::pair<std::string, std::string>> otherWindows;
    {
      std::ifstream ifs(intoFile, std::ios::in | std::ios::binary);
      std::string line, key, value, windowField;
      uint32_t windowInstance{0};
      while (ifs.is_open() && std::getline(ifs, line))
      {
        if (splitLine(line, key, value) && isWindowKey(key, windowField, windowInstance) &&
            windowInstance != instance)
        {
          otherWindows.emplace_back(key, value);
        }
      }
    }

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
      ofs << windowKey("windowX", instance) << "=" << windowX << "\n";
      ofs << windowKey("windowY", instance) << "=" << windowY << "\n";
      ofs << windowKey("windowWidth", instance) << "=" << windowWidth << "\n";
      ofs << windowKey("windowHeight", instance) << "=" << windowHeight << "\n";
    }

    for (const auto &[key, value] : otherWindows)
    {
      ofs << key << "=" << escape(value) << "\n";
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
