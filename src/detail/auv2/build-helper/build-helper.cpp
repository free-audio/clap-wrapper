#include <iostream>
#include <fstream>
#include <vector>

#include "detail/clap/fsutil.h"

#if MACOS_USE_STD_FILESYSTEM
#include <filesystem>
namespace fs = std::filesystem;
#else
#include "ghc/filesystem.hpp"
namespace fs = ghc::filesystem;
#endif

struct auInfo
{
    std::string name, vers, type, subt, manu, manunm, clapid, desc;

    const std::string factoryBase{"wrapAsAUV2_inst"};

    void writePListFragment(std::ostream &of, int idx) const
    {
        if (!clapid.empty())
        {
            of << "      <!-- entry for id '" << clapid << "' / index " << idx << " -->\n";
        }
        else
        {
            of << "      <!-- entry for index " << idx << " clap id unknown -->\n";
        }
        of << "      <dict>\n"
           << "        <key>name</key>\n"
           << "        <string>" << manunm << ": " << name << "</string>\n"
           << "        <key>description</key>\n"
           << "        <string>" << desc << "</string>\n"
           << "        <key>factoryFunction</key>\n"
           << "        <string>" << factoryBase << idx << "Factory"
           << "</string>\n"
           << "        <key>manufacturer</key>\n"
           << "        <string>" << manu << "</string>\n"
           << "        <key>subtype</key>\n"
           << "        <string>" << subt << "</string>\n"
           << "        <key>type</key>\n"
           << "        <string>" << type << "</string>\n"
           << "        <key>version</key>\n"
           << "        <integer>1</integer>\n"
           << "        <key>sandboxSafe</key>\n"
           << "        <true/>\n"
           << "        <key>resourceUsage</key>\n"
           << "        <dict>\n"
           << "           <key>network.client</key>\n"
           << "           <true/>\n"
           << "           <key>temporary-exception.files.all.read-write</key>\n"
           << "           <true/>\n"
           << "        </dict>\n"
           << "      </dict>\n";
    }
};

bool buildUnitsFromClap(const std::string &clapfile, std::string &manu, std::string &manuName, std::vector<auInfo> &units)
{
    Clap::Library loader;
    if (!loader.load(clapfile.c_str()))
    {
        std::cout << "[ERROR] library.load of clapfile failed" << std::endl;
        return false;
    }

    int idx{0};
    for (const auto *clapPlug : loader.plugins)
    {
        auto u = auInfo();
        u.name = clapPlug->name;
        u.clapid = clapPlug->id;
        u.vers = clapPlug->version;
        u.desc = clapPlug->description;

        u.subt = "FIX" + std::to_string(idx++);
        u.manu = manu;
        u.manunm = manuName;


        auto f = clapPlug->features[0];
        if (f == nullptr || strcmp(f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0)
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


        units.push_back(u);
    }
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 1;

    std::cout << "clap-wrapper: auv2 configuration tool starting\n";

    std::vector<auInfo> units;
    if (std::string(argv[1]) == "--explicit")
    {
        if (argc != 8)
        {
            std::cout << "[ERROR] Configuration incorrect. Got " << argc << " arguments in explicit" << std::endl;
            return 5;
        }
        int idx = 2;
        auInfo u;
        u.name = std::string(argv[idx++]);
        u.vers = std::string(argv[idx++]);
        u.type = std::string(argv[idx++]);
        u.subt = std::string(argv[idx++]);
        u.manu = std::string(argv[idx++]);
        u.manunm = std::string(argv[idx++]);
        u.desc = u.name + " CLAP to AU Wrapper";

        std::cout << "  - single plugin explicit mode: " << u.name << " (" << u.type << "/" << u.subt << ")" << std::endl;
        units.push_back(u);
    }
    else if (std::string(argv[1]) == "--fromclap")
    {
        if (argc != 5)
        {
            std::cout << "[ERROR] Configuration incorrect. Got " << argc << " arguments in fromclap" << std::endl;
            return 5;
        }
        int idx = 2;
        auto clapfile = std::string(argv[idx++]);
        auto mcode = std::string(argv[idx++]);
        auto mname = std::string(argv[idx++]);

        try {
            auto p = fs::path{clapfile};
            // This is a hack for now - we get to the dll
            p = p.parent_path().parent_path().parent_path();
            clapfile = p.u8string();
        }
        catch (const fs::filesystem_error &e)
        {
            std::cout << "[ERROR] cant get path " << e.what() << std::endl;
            return 3;
        }

        std::cout << "  - building information from CLAP directly\n"
                  << "  - source clap: '" << clapfile << "'" << std::endl;

        if (!buildUnitsFromClap(clapfile, mcode, mname, units))
        {
            std::cout << "[ERROR] Can't build units from CLAP" << std::endl;
            return 4;
        }

        if (units.empty())
        {
            std::cout << "[ERROR] No units from clap file\n";
            return 5;
        }

        std::cout << "  - clap file produced " << units.size() << " units" << std::endl;

    }
    else
    {
        std::cout << "[ERROR] Unknown Mode : " << argv[1] << std::endl;
        return 2;
    }

    std::cout << "  - generating auv2_Info.plist from auv2_infoplist_top" << std::endl;
    std::ifstream intop("auv2_infoplist_top");
    if (!intop.is_open())
    {
        std::cerr << "[ERROR] Unable to open pre-generated file auv2_infoplist_top" << std::endl;
        return 1;
    }

    std::ofstream of("auv2_Info.plist");
    if (!of.is_open())
    {
        std::cerr << "[ERROR] Unable to open output file auv2_Info.plist" << std::endl;
    }
    of << intop.rdbuf();

    of << "    <key>AudioComponents</key>\n    <array>\n";
    int idx{0};
    for (const auto &u : units)
    {
        std::cout << "    + " << u.name << " (" << u.type << "/" << u.subt << ") by " << u.manunm
                  << " (" << u.manu << ")" << std::endl;
        u.writePListFragment(of, idx++);
    }
    of << "    </array>\n";
    of << "  </dict>\n</plist>\n";
    of.close();
    std::cout << "  - auv2_Info.plist generated" << std::endl;

    std::cout << "  - generating generated_entrypoints.hxx" << std::endl;
    std::ofstream cppf("generated_entrypoints.hxx");
    if (!cppf.is_open())
    {
        std::cout << "[ERROR] Unable to open generated_endpoints.hxx" << std::endl;
        return 1;
    }

    cppf << "#pragma once\n";
    cppf << "#include \"detail/auv2/base_classes.h\"\n\n";

    idx = 0;
    for (const auto &u : units)
    {
        auto on = u.factoryBase + std::to_string(idx);

        if (u.type == "aumu")
        {
            std::cout << "    + " << u.name << " entry " << on << " from ClapWrapper_AUV2_Instrument" << std::endl;
            cppf << "struct " << on << " : ClapWrapper_AUV2_Instrument {\n"
                 << "   " << on << "(AudioComponentInstance ci) :\n"
                 << "         ClapWrapper_AUV2_Instrument(\"" << u.clapid << "\", " << idx << ", ci) {}"
                 << "};\n"
                 << "AUSDK_COMPONENT_ENTRY(ausdk::AUMusicDeviceFactory, " << on << ");\n";
        }
        else if (u.type == "aumi")
        {
            std::cout << "    + " << u.name << " entry " << on << " from ClapWrapper_AUV2_NoteEffect" << std::endl;
            cppf << "struct " << on << " : ClapWrapper_AUV2_NoteEffect {\n"
                 << "   " << on << "(AudioComponentInstance ci) :\n"
                 << "         ClapWrapper_AUV2_NoteEffect(\"" << u.clapid << "\", " << idx << ", ci) {}"
                 << "};\n"
                 << "AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory , " << on << ");\n";
        }
        else if (u.type == "aufx")
        {
            std::cout << "    + " << u.name << " entry " << on << " from ClapWrapper_AUV2_Effect" << std::endl;
            cppf << "struct " << on << " : ClapWrapper_AUV2_Effect {\n"
                 << "   " << on << "(AudioComponentInstance ci) :\n"
                 << "         ClapWrapper_AUV2_Effect(\"" << u.clapid << "\", " << idx << ", ci) {}"
                 << "};\n"
                 << "AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, " << on << ");\n";
        }

        idx++;
    }
    cppf.close();
    std::cout << "  - generated_entrypoints.hxx generated" << std::endl;

    return 0;
}