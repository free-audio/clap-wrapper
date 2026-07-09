#include <algorithm>
#include <iostream>
#include <fstream>
#include <vector>
#include <functional>
#include <sstream>

#include "detail/clap/fsutil.h"
#include "detail/os/fs.h"
#include "detail/shared/util.h"

// Generate a 4-character FourCC string from an arbitrary input string
// using a deterministic hash. Same approach as AAXIDfromString.
static const char _fourcc_map[65] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz$_";
static std::string fourCCFromString(const std::string &input)
{
  uint32_t p = fnv1a_keogh(input.c_str());
  char result[5];
  result[0] = _fourcc_map[(p >> 0) & 0x3f];
  result[1] = _fourcc_map[(p >> 6) & 0x3f];
  result[2] = _fourcc_map[(p >> 12) & 0x3f];
  result[3] = _fourcc_map[(p >> 18) & 0x3f];
  result[4] = 0;
  return result;
}

// Generate a valid ObjC identifier suffix from an arbitrary string.
// Uses hex encoding of fnv1a hash to produce a unique, deterministic suffix.
static std::string objcIdentifierFromString(const std::string &input)
{
  uint32_t hash = fnv1a_keogh(input.c_str());
  char buf[16];
  snprintf(buf, sizeof(buf), "%08X", hash);
  return buf;
}

// Escape a string for use in an XML text node. Plugin names/descriptions
// come straight out of the CLAP descriptor — "Drums & Bass" written raw
// produces an unparseable Info.plist and the component silently never
// registers.
static std::string escapeXML(const std::string &input)
{
  std::string out;
  out.reserve(input.size());
  for (char c : input)
  {
    switch (c)
    {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&apos;";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

// Escape a string for use inside an ObjC @"..." literal in generated code.
static std::string escapeObjCString(const std::string &input)
{
  std::string out;
  out.reserve(input.size());
  for (char c : input)
  {
    if (c == '\\' || c == '"') out += '\\';
    if (c == '\n')
    {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out;
}

struct auInfo
{
  std::string name, vers, type, subt, manu, manunm, clapid, desc, clapname, bundlevers;
  bool explicitMode{false};
  std::vector<std::string> tags;

  // Each plugin needs a unique ObjC class name to avoid collisions when
  // multiple AUv3 wrappers are loaded in the same process.
  std::string factoryBase() const
  {
    std::string key = manu + subt;
    if (!clapid.empty()) key = clapid;
    return "ClapAUv3VC_" + objcIdentifierFromString(key);
  }

  uint32_t bundleversToVersion() const
  {
    // "major[.minor[.patch]]" -> 0x00MMmmpp. Every component including the
    // last must be parsed — "1.2.3" is 0x010203, so each patch release
    // yields a distinct AudioComponent version and hosts detect the update.
    // Each component is clamped to its byte, otherwise a build-number-style
    // patch ("1.2.300") carries into the minor byte and version ordering
    // across releases becomes wrong.
    uint16_t rev[3]{0, 0, 0};
    auto uv = bundlevers;
    for (int i = 0; i < 3 && !uv.empty(); ++i)
    {
      rev[i] = (uint16_t)std::min(std::max(std::atoi(uv.c_str()), 0), 255);
      auto p = uv.find('.');
      if (p == std::string::npos) break;
      uv = uv.substr(p + 1);
    }
    return std::max((rev[0] << 16) + (rev[1] << 8) + rev[2], 1);
  }

  void writePListFragment(std::ostream &of, int idx) const
  {
    if (!clapid.empty())
    {
      // XML comments are invalidated by the sequence "--" (which escapeXML
      // cannot represent) — break it up.
      auto commentSafe = escapeXML(clapid);
      size_t dd;
      while ((dd = commentSafe.find("--")) != std::string::npos) commentSafe.replace(dd, 2, "- -");
      of << "          <!-- entry for id '" << commentSafe << "' / index " << idx << " -->\n";
    }
    else
    {
      of << "          <!-- entry for index " << idx << " clap id unknown -->\n";
    }
    of << "          <dict>\n"
       << "            <key>name</key>\n"
       << "            <string>" << escapeXML(manunm) << ": " << escapeXML(name) << "</string>\n"
       << "            <key>description</key>\n"
       << "            <string>" << escapeXML(desc) << "</string>\n"
       << "            <key>factoryFunction</key>\n"
       << "            <string>" << factoryBase() << idx << "</string>\n"
       << "            <key>manufacturer</key>\n"
       << "            <string>" << escapeXML(manu) << "</string>\n"
       << "            <key>subtype</key>\n"
       << "            <string>" << escapeXML(subt) << "</string>\n"
       << "            <key>type</key>\n"
       << "            <string>" << escapeXML(type) << "</string>\n"
       << "            <key>version</key>\n"
       << "            <integer>" << bundleversToVersion() << "</integer>\n"
       << "            <key>sandboxSafe</key>\n"
       << "            <true/>\n";
    // No resourceUsage dict by default: granting network.client or file
    // exceptions to every wrapped plugin contradicts sandboxSafe and is
    // grounds for App Store rejection. When a hosted CLAP genuinely needs
    // extra sandbox powers, thread a per-plugin option through here
    // instead of widening this default.

    if (!tags.empty())
    {
      of << "            <key>tags</key>\n"
         << "            <array>\n";
      for (auto tag : tags)
      {
        if (tag[0] >= 'a' && tag[0] <= 'z')
        {
          tag[0] = std::toupper(tag[0]);
        }
        of << "              <string>" << escapeXML(tag) << "</string>\n";
      }
      of << "            </array>\n";
    }
    of << "          </dict>\n";
  }
};

bool buildUnitsFromClap(const std::string &clapfile, const std::string &clapname, std::string manu,
                        std::string manuName, std::string itype, std::string subt,
                        std::vector<auInfo> &units)
{
  Clap::Library loader;
  if (!loader.load(clapfile))
  {
    std::cout << "[ERROR] library.load of clapfile failed" << std::endl;
    return false;
  }

  int idx{0};

  if (manu.empty() && loader._pluginFactoryAUv2Info == nullptr)
  {
    std::cout << "[ERROR] No manufacturer provider and no auv2 info available" << std::endl;
    return false;
  }

  if (manu.empty())
  {
    manu = loader._pluginFactoryAUv2Info->manufacturer_code;
    manuName = loader._pluginFactoryAUv2Info->manufacturer_name;
    std::cout << "  - using factory manufacturer '" << manuName << "' (" << manu << ")" << std::endl;
  }

  if (!itype.empty() && !subt.empty() && loader.plugins.size() > 1)
  {
    std::cout << "[ERROR] Multi-plugin claps must specify itype and subtype via extension" << std::endl;
  }

  for (const auto *clapPlug : loader.plugins)
  {
    auto u = auInfo();
    bool doExport = true;

    u.name = clapPlug->name;
    u.clapname = clapname;
    u.clapid = clapPlug->id;
    u.vers = clapPlug->version;
    u.desc = clapPlug->description;

    // Generate a deterministic FourCC subtype from manufacturer + plugin id
    // using the fnv1a hash (same approach as AAX wrapper)
    if (subt.empty())
    {
      std::string hashInput = manu + ":" + std::string(clapPlug->id);
      u.subt = fourCCFromString(hashInput);
      std::cout << "  - generated subtype '" << u.subt << "' from '" << hashInput << "'" << std::endl;
    }
    else
    {
      u.subt = subt;
    }
    u.manu = manu;
    u.manunm = manuName;

    auto f = clapPlug->features[0];
    if (!itype.empty())
    {
      u.type = itype;
    }
    else if (f == nullptr || strcmp(f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0)
    {
      u.type = "aumu";
    }
    else if (strcmp(f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0)
    {
      u.type = "aufx";
    }
    else if (strcmp(f, CLAP_PLUGIN_FEATURE_NOTE_EFFECT) == 0)
    {
      u.type = "aumi";
    }
    else
    {
      std::cout << "[WARNING] can't determine instrument type. Using aumu" << std::endl;
      u.type = "aumu";
    }

    auto fp = clapPlug->features;
    while (*fp)
    {
      u.tags.push_back(*fp);
      ++fp;
    }

    if (loader._pluginFactoryAUv2Info)
    {
      clap_plugin_info_as_auv2_t v2inf;
      auto res =
          loader._pluginFactoryAUv2Info->get_auv2_info(loader._pluginFactoryAUv2Info, idx, &v2inf);
      if (res)
      {
        if (v2inf.au_type[0] != 0)
        {
          u.type = v2inf.au_type;
        }
        if (v2inf.au_subt[0] != 0)
        {
          u.subt = v2inf.au_subt;
        }
      }
      else
      {
        doExport = false;
        std::cout << "  - Skipping Audio Unit Export for index " << idx << "/" << u.clapid << std::endl;
      }
    }

    if (doExport)
    {
      units.push_back(u);
    }
    idx++;
  }
  return true;
}

int main(int argc, char **argv)
{
  if (argc < 2) return 1;

  std::cout << "clap-wrapper: auv3 configuration tool starting\n";

  std::vector<auInfo> units;
  if (std::string(argv[1]) == "--explicit")
  {
    if (argc != 8)
    {
      std::cout << "[ERROR] Configuration incorrect. Got " << argc << " arguments in explicit"
                << std::endl;
      return 5;
    }
    int idx = 2;
    auInfo u;
    u.explicitMode = true;
    u.name = std::string(argv[idx++]);
    u.clapname = u.name;
    u.vers = std::string(argv[idx++]);
    u.bundlevers = u.vers;
    u.type = std::string(argv[idx++]);
    u.subt = std::string(argv[idx++]);
    u.manu = std::string(argv[idx++]);
    u.manunm = std::string(argv[idx++]);
    u.desc = u.name + " CLAP to AUv3 Wrapper";

    std::cout << "  - single plugin explicit mode: " << u.name << " (" << u.type << "/" << u.subt << ")"
              << std::endl;
    units.push_back(u);
  }
  else if (std::string(argv[1]) == "--fromclap")
  {
    if (argc < 4)
    {
      std::cout << "[ERROR] Configuration incorrect. Got " << argc << " arguments in fromclap"
                << std::endl;
      return 6;
    }
    int idx = 2;
    auto clapname = std::string(argv[idx++]);
    auto clapfile = std::string(argv[idx++]);
    auto bundlev = std::string(argv[idx++]);

    auto nerr = [](const auto &a)
    {
      if (a == "errr")
        return std::string();
      else
        return a;
    };

    auto mcode = nerr((idx < argc) ? std::string(argv[idx++]) : std::string());
    auto mname = nerr((idx < argc) ? std::string(argv[idx++]) : std::string());

    auto itype = nerr((idx < argc) ? std::string(argv[idx++]) : std::string());
    auto isubt = nerr((idx < argc) ? std::string(argv[idx++]) : std::string());

    try
    {
      auto p = fs::path{clapfile};
      if (fs::is_directory(p))
      {
        std::cout << "  - CLAP is a directory. Assuming bundle\n";
      }
      else
      {
        std::cout << "  - CLAP is a regular file. Assuming dll in bundle\n";
        p = p.parent_path().parent_path().parent_path();
        clapfile = p.u8string();
      }
    }
    catch (const fs::filesystem_error &e)
    {
      std::cout << "[ERROR] cant get path " << e.what() << std::endl;
      return 3;
    }

    std::cout << "  - building information from CLAP directly\n"
              << "  - source clap: '" << clapfile << "'" << std::endl;

    if (!buildUnitsFromClap(clapfile, clapname, mcode, mname, itype, isubt, units))
    {
      std::cout << "[ERROR] Can't build units from CLAP" << std::endl;
      return 4;
    }

    if (units.empty())
    {
      std::cout << "[ERROR] No units from clap file\n";
      return 5;
    }

    for (auto &u : units)
    {
      u.bundlevers = bundlev;
    }

    std::cout << "  - clap file produced " << units.size() << " units" << std::endl;
  }
  else
  {
    std::cout << "[ERROR] Unknown Mode : " << argv[1] << std::endl;
    return 2;
  }

  // --- Generate auv3_Info.plist ---
  std::cout << "  - generating auv3_Info.plist from auv3_infoplist_top" << std::endl;
  std::ifstream intop("auv3_infoplist_top");
  if (!intop.is_open())
  {
    std::cerr << "[ERROR] Unable to open pre-generated file auv3_infoplist_top" << std::endl;
    return 1;
  }

  std::ofstream of("auv3_Info.plist");
  if (!of.is_open())
  {
    std::cerr << "[ERROR] Unable to open output file auv3_Info.plist" << std::endl;
    return 1;
  }
  of << intop.rdbuf();

  // The principal class is the first factory subclass
  std::string principalClass = units[0].factoryBase() + "0";

  of << "    <key>NSExtension</key>\n"
     << "    <dict>\n"
     << "      <key>NSExtensionPointIdentifier</key>\n"
     << "      <string>com.apple.AudioUnit-UI</string>\n"
     << "      <key>NSExtensionPrincipalClass</key>\n"
     << "      <string>" << principalClass << "</string>\n"
     << "      <key>NSExtensionAttributes</key>\n"
     << "      <dict>\n";

  of << "        <key>AudioComponents</key>\n        <array>\n";
  int idx{0};
  for (const auto &u : units)
  {
    std::cout << "    + " << u.name << " (" << u.type << "/" << u.subt << ") by " << u.manunm << " ("
              << u.manu << ")" << std::endl;
    u.writePListFragment(of, idx++);
  }
  of << "        </array>\n";
  of << "      </dict>\n";  // close NSExtensionAttributes
  of << "    </dict>\n";    // close NSExtension
  of << "  </dict>\n</plist>\n";
  of.close();
  std::cout << "  - auv3_Info.plist generated" << std::endl;

  // --- Generate generated_auv3_entrypoints.hxx ---
  {
    std::cout << "  - generating generated_auv3_entrypoints.hxx" << std::endl;
    std::ofstream cppf("generated_auv3_entrypoints.hxx");
    if (!cppf.is_open())
    {
      std::cout << "[ERROR] Unable to open generated_auv3_entrypoints.hxx" << std::endl;
      return 1;
    }

    cppf << "#pragma once\n\n";
    cppf << "// Generated by AUv3 build helper - do not edit\n\n";
    cppf << "#import \"detail/auv3/auv3_audiounit.h\"\n\n";

    idx = 0;
    for (const auto &u : units)
    {
      auto vcName = u.factoryBase() + std::to_string(idx);

      std::cout << "    + " << u.name << " view controller " << vcName << std::endl;

      // Generate a unique AUViewController subclass per plugin
      // This class serves as both the view controller AND the AUAudioUnitFactory.
      // Escape the comment too — a newline in a descriptor string would
      // otherwise spill the rest of the name out of the // comment as code.
      cppf << "// ViewController/Factory for '" << escapeObjCString(u.name) << "' ("
           << escapeObjCString(u.type) << "/" << escapeObjCString(u.subt) << ")\n";
      cppf << "@interface " << vcName << " : ClapAUv3ViewController\n"
           << "@end\n\n";
      cppf << "@implementation " << vcName << "\n";
      cppf
          << "- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc\n"
          << "                                                   error:(NSError **)error {\n"
          << "    ClapAUv3AudioUnit *au = [[ClapAUv3AudioUnit alloc] initWithComponentDescription:desc\n"
          << "                                                          options:0\n"
          << "                                                            error:error\n"
          << "                                                         clapName:@\""
          << escapeObjCString(u.clapname) << "\"\n"
          << "                                                           clapId:@\""
          << escapeObjCString(u.clapid) << "\"\n"
          << "                                                        clapIndex:" << idx << "];\n"
          << "    self.audioUnit = au;\n"
          << "    return au;\n"
          << "}\n"
          << "@end\n\n";

      idx++;
    }
    cppf.close();
    std::cout << "  - generated_auv3_entrypoints.hxx generated" << std::endl;
  }

  return 0;
}
