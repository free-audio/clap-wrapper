/*

		CLAP AS VST3 - Entrypoint

		Copyright (c) 2022 Timo Kaluza (defiantnerd)

		This file is part of the clap-wrappers project which is released under MIT License.
		See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

		Provides the entry function for the VST3 flavor of the wrapped plugin.

		When the VST3 factory is being scanned, this tries to locate a clap_entry function
		in the following order and stops if a .clap binary has been found:

		1) checks for exported `clap_enty` in this binary itself (statically linked wrapper)
		2) determines it's own filename without the .vst3 ending and determines a list of all valid CLAP search paths (see below) and
		   a) checks each CLAP search path for a matching .clap
			 b) checks it's own parent folder name and tries to add it to the .clap path. This allows a vst3 wrapper placed in
					{any VST3 Folder}/mevendor/myplugin.vst3 to match {any CLAP folder}/mevendor/myplugin.clap
			 c) checks all subfolders in the CLAP folders for a matching .clap.
		
		Valid CLAP search paths are also documented in clap/include/clap/entry.h:

		// CLAP plugins standard search path:

    Linux
      - ~/.clap
      - /usr/lib/clap
   
    Windows
      - %CommonFilesFolder%/CLAP/
      - %LOCALAPPDATA%/Programs/Common/CLAP/
   
    MacOS
      - /Library/Audio/Plug-Ins/CLAP
      - ~/Library/Audio/Plug-Ins/CLAP
   
    In addition to the OS-specific default locations above, a CLAP host must query the environment
    for a CLAP_PATH variable, which is a list of directories formatted in the same manner as the host
    OS binary search path (PATH on Unix, separated by `:` and Path on Windows, separated by ';', as
    of this writing).
   
    Each directory should be recursively searched for files and/or bundles as appropriate in your OS
    ending with the extension `.clap`.


*/
#include "detail/sha1.h"
#include "wrapasvst3.h"
#include "public.sdk/source/main/pluginfactory.h"
#include <array>

using namespace Steinberg::Vst;

//------------------------------------------------------------------------
//  VST Plug-in Entry
//------------------------------------------------------------------------
// Windows: do not forget to include a .def file in your project to export
// GetPluginFactory function!
//------------------------------------------------------------------------

#include "detail/clap/fsutil.h"
#include "detail/vst3/categories.h"
#include "clap_proxy.h"

struct CreationContext
{
  Clap::Library* lib = nullptr;
  int index = 0;
  PClassInfo2 classinfo;
};

bool findPlugin(Clap::Library& lib, const std::string& pluginfilename)
{
  auto parentfolder = os::getParentFolderName();
  auto paths = Clap::getValidCLAPSearchPaths();

  // Strategy 1: look for a clap with the same name as this binary
  for (auto& i : paths)
  {
    if (!fs::exists(i)) continue;
    // try to find it the CLAP folder immediately
    auto k1 = i / pluginfilename;
    LOGDETAIL("scanning for binary: {}", k1.u8string().c_str());

    if (fs::exists(k1))
    {
      if (lib.load(k1.u8string().c_str()))
      {
        return true;
      }
    }

    // Strategy 2: try to locate "CLAP/vendorX/plugY.clap"  - derived from "VST3/vendorX/plugY.vst3"
    auto k2 = i / parentfolder / pluginfilename;
    LOGDETAIL("scanning for binary: {}", k2.u8string().c_str());
    if (fs::exists(k2))
    {
      if (lib.load(k2.u8string().c_str()))
      {
        return true;
      }
    }

    // Strategy 3: enumerate folders in CLAP folder and try to locate the plugin in any sub folder (only one level)
    for (const auto& subdir : fs::directory_iterator(i))
    {
      auto k3 = i / subdir / pluginfilename;
      LOGDETAIL("scanning for binary: {}", k3.u8string().c_str());
      if (fs::exists(k3))
      {
        if (lib.load(k3.u8string().c_str()))
        {
          return true;
        }
      }
    }
  }

  return false;
}

IPluginFactory* GetPluginFactoryEntryPoint()
{
#if _DEBUG
  // MessageBoxA(NULL,"halt","me",MB_OK); // <- enable this on Windows to get a debug attachment to vstscanner.exe (subprocess of cbse)
#endif

#if SMTG_OS_WINDOWS
// #pragma comment(linker, "/EXPORT:" __FUNCTION__ "=" __FUNCDNAME__)
#endif

  // static IPtr<Steinberg::CPluginFactory> gPluginFactory = nullptr;
  static Clap::Library gClapLibrary;

  static std::vector<CreationContext> gCreationContexts;

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

  if (!gPluginFactory)
  {
    // we need at least one plugin to obtain vendor/name etc.
    auto* factoryvendor = gClapLibrary.plugins[0]->vendor;
    auto* vendor_url = gClapLibrary.plugins[0]->url;
    // TODO: extract the domain and prefix with info@
    auto* contact = "info@";

    // override for VST3 specifics
    if (gClapLibrary._pluginFactoryVst3Info)
    {
      LOGDETAIL("detected extension `{}`", CLAP_PLUGIN_FACTORY_INFO_VST3);
      auto& v3 = gClapLibrary._pluginFactoryVst3Info;
      if (v3->vendor) factoryvendor = v3->vendor;
      if (v3->vendor_url) vendor_url = v3->vendor_url;
      if (v3->email_contact) contact = v3->email_contact;
    }

    static PFactoryInfo factoryInfo(factoryvendor, vendor_url, contact, Vst::kDefaultFactoryFlags);

    LOGDETAIL("created factory for vendor '{}'", factoryvendor);

    gPluginFactory = new Steinberg::CPluginFactory(factoryInfo);
    // resize the classInfo vector
    gCreationContexts.clear();
    gCreationContexts.reserve(gClapLibrary.plugins.size());
    int numPlugins = static_cast<int>(gClapLibrary.plugins.size());
    LOGDETAIL("number of plugins in factory: {}", numPlugins);
    for (int ctr = 0; ctr < numPlugins; ++ctr)
    {
      auto& clapdescr = gClapLibrary.plugins[ctr];
      auto vst3info = gClapLibrary.get_vst3_info(ctr);

      LOGDETAIL("  plugin #{}: '{}'", ctr, clapdescr->name);

      std::string n(clapdescr->name);
#ifdef _DEBUG
      n.append(" (CLAP->VST3)");
#endif
      auto plugname = n.c_str();  //  clapdescr->name;

      // get vendor -------------------------------------
      auto pluginvendor = clapdescr->vendor;
      if (pluginvendor == nullptr || *pluginvendor == 0) pluginvendor = "Unspecified Vendor";
      if (vst3info && vst3info->vendor)
      {
        LOGDETAIL("  plugin supports extension '{}'", CLAP_PLUGIN_AS_VST3);
        pluginvendor = vst3info->vendor;
      }

      TUID lcid;
      Crypto::uuid_object g;

#ifdef CLAP_VST3_TUID_STRING
      Steinberg::FUID f;
      if (f.fromString(CLAP_VST3_TUID_STRING))
      {
        memcpy(&g, f.toTUID(), sizeof(TUID));
        memcpy(&lcid, &g, sizeof(TUID));
      }
      else
#endif
      {
        // make id or take it from vst3 info --------------
        std::string id(clapdescr->id);
        if (vst3info && vst3info->componentId)
        {
          memcpy(&g, vst3info->componentId, sizeof(g));
        }
        else
        {
          g = Crypto::create_sha1_guid_from_name(id.c_str(), id.size());
        }

        memcpy(&lcid, &g, sizeof(TUID));
      }

      // features ----------------------------------------
      std::string features;
      if (vst3info && vst3info->features)
      {
        features = vst3info->features;
      }
      else
      {
        features = clapCategoriesToVST3(clapdescr->features);
      }

#if CLAP_WRAPPER_LOGLEVEL > 1
      {
        const uint8_t* v = reinterpret_cast<const uint8_t*>(&g);
        char x[sizeof(g) * 2 + 8];
        char* o = x;
        constexpr char hexchar[] = "0123456789ABCDEF";
        for (auto i = 0U; i < sizeof(g); i++)
        {
          auto n = v[i];
          *o++ = hexchar[(n >> 4) & 0xF];
          *o++ = hexchar[n & 0xF];
          if (!(i % 4)) *o++ = 32;
        }
        *o++ = 0;
        LOGDETAIL("plugin id: {} -> {}", clapdescr->id, x);
      }
#endif

      gCreationContexts.push_back(
          {&gClapLibrary, ctr,
           PClassInfo2(
               lcid, PClassInfo::kManyInstances, kVstAudioEffectClass, plugname,
               0 /* the only flag is usually Vst:kDistributable, but CLAPs aren't distributable */,
               features.c_str(), pluginvendor, clapdescr->version, kVstVersionString)});
      gPluginFactory->registerClass(&gCreationContexts.back().classinfo, ClapAsVst3::createInstance,
                                    &gCreationContexts.back());
    }
  }
  else
    gPluginFactory->addRef();

  return gPluginFactory;
}

/*
*		creates an Instance from the creationContext.
*		actually, there is always a valid entrypoint, otherwise no factory would have been provided.
*/
FUnknown* ClapAsVst3::createInstance(void* context)
{
  auto ctx = static_cast<CreationContext*>(context);

  LOGINFO("creating plugin {} (#{})", ctx->classinfo.name, ctx->index);
  if (ctx->lib->hasEntryPoint())
  {
    // MessageBoxA(NULL, "halt", "create", MB_OK);
    return (IAudioProcessor*)new ClapAsVst3(ctx->lib, ctx->index, context);
  }
  return nullptr;  // this should never happen.
}
