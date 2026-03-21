#include "factory.h"

#include "AAX.h"
#include "AAX_IEffectDescriptor.h"

namespace CLAPAAX
{
static Clap::Library gClapLibrary;

bool findPlugin(Clap::Library &lib, const std::string &pluginfilename)
{
  auto parentfolder = os::getParentFolderName();
  auto paths = Clap::getValidCLAPSearchPaths();

  // Strategy 1: look for a clap with the same name as this binary
  for (auto &i : paths)
  {
    if (!fs::exists(i)) continue;
    // try to find it the CLAP folder immediately
    auto k1 = i / pluginfilename;
    LOGDETAIL("scanning for binary: {}", k1.u8string().c_str());

    if (fs::exists(k1))
    {
      if (lib.load(k1))
      {
        return true;
      }
    }

    // Strategy 2: try to locate "CLAP/vendorX/plugY.clap"  - derived from "VST3/vendorX/plugY.vst3"
    auto k2 = i / parentfolder / pluginfilename;
    LOGDETAIL("scanning for binary: {}", k2.u8string().c_str());
    if (fs::exists(k2))
    {
      if (lib.load(k2))
      {
        return true;
      }
    }

    // Strategy 3: enumerate folders in CLAP folder and try to locate the plugin in any sub folder (only one level)
    for (const auto &subdir : fs::directory_iterator(i))
    {
      auto k3 = i / subdir / pluginfilename;
      LOGDETAIL("scanning for binary: {}", k3.u8string().c_str());
      if (fs::exists(k3))
      {
        if (lib.load(k3))
        {
          return true;
        }
      }
    }
  }

  return false;
}

Clap::Library *guarantee_clap()
{
#if 1
  // if there is no ClapLibrary yet
  if (!gClapLibrary._pluginFactory)
  {
    // if this binary does not already contain a CLAP entrypoint
    if (!gClapLibrary.hasEntryPoint())
    {
      // try to find a clap which filename stem matches our own
      auto kx = os::getParentFolderName();
      auto plugname = os::getBinaryName();
      plugname.append(".clap");

      if (!findPlugin(gClapLibrary, plugname))
      {
        return nullptr;
      }
    }
    else
    {
      LOGDETAIL("detected entrypoint in this binary");
    }
  }
  if (gClapLibrary.plugins.empty())
  {
    // with no plugins there is nothing to do..
    LOGINFO("no plugin has been found");
    return nullptr;
  }

  if (!clap_version_is_compatible(gClapLibrary.plugins[0]->clap_version))
  {
    // CLAP version is not compatible -> eject
    LOGINFO("CLAP version is not compatible");
    return nullptr;
  }
#endif
  return &gClapLibrary;
}

}  // namespace CLAPAAX
