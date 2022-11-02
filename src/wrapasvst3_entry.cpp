/*

		CLAP AS VST3 - Entrypoint

		Copyright (c) 2022 Timo Kaluza (defiantnerd)

		This file is part of the clap-wrappers project which is released under MIT License.
		See file LICENSE or go to https://github.com/defiantnerd/clap-wrapper for full license details.

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
#include <iostream>

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
		if ( !std::filesystem::exists(i) )
		 continue;
		// try to find it the CLAP folder immediately
		auto k1 = i / pluginfilename;
		std::cout << "scanning for " << k1 << std::endl;
		
		if (std::filesystem::exists(k1))
		{
			if (lib.load(k1.u8string().c_str()))
			{
				return true;
			}
		}

		// Strategy 2: try to locate "CLAP/vendorX/plugY.clap"  - derived from "VST3/vendorX/plugY.vst3"
		auto k2 = i / parentfolder / pluginfilename;
		if (std::filesystem::exists(k2))
		{
			if (lib.load(k2.u8string().c_str()))
			{
				return true;
			}
		}

		// Strategy 3: enumerate folders in CLAP folder and try to locate the plugin in any sub folder (only one level)
		for (const auto& subdir : std::filesystem::directory_iterator(i))
		{
			auto k3 = i / subdir / pluginfilename;
			if (std::filesystem::exists(k3))
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

SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {

// MessageBoxA(NULL,"halt","me",MB_OK); <- enable this on Windows to get a debug attachment to vstscanner.exe (subprocess of cbse)
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
	}
	if (gClapLibrary.plugins.empty())
	{
		// with no plugins there is nothing to do..
		return nullptr;
	}
	if (!clap_version_is_compatible(gClapLibrary.plugins[0]->clap_version))
	{
		// CLAP version is not compatible -> eject
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
			auto& v3 = gClapLibrary._pluginFactoryVst3Info;
			if (v3->vendor) factoryvendor = v3->vendor;
			if (v3->vendor_url) vendor_url = v3->vendor_url;
			if (v3->email_contact) contact = v3->email_contact;
		}

		static PFactoryInfo factoryInfo(
			factoryvendor, 
			vendor_url, 
			contact,
			Vst::kDefaultFactoryFlags);

		gPluginFactory = new Steinberg::CPluginFactory(factoryInfo);		
		// resize the classInfo vector
		gCreationContexts.reserve(gClapLibrary.plugins.size());
		int numPlugins = static_cast<int>(gClapLibrary.plugins.size());
		for (int ctr = 0; ctr < numPlugins; ++ctr)
		{
			auto& clapdescr = gClapLibrary.plugins[ctr];
			auto vst3info = gClapLibrary.get_vst3_info(ctr);

			std::string n(clapdescr->name);
#ifdef _DEBUG
			n.append(" (CLAP->VST3)");
#endif
			auto plugname = n.c_str(); //  clapdescr->name;

			// get vendor -------------------------------------
			auto pluginvendor = clapdescr->vendor;
			if (pluginvendor == nullptr || *pluginvendor == 0) pluginvendor = "Unspecified Vendor";
			if (vst3info && vst3info->vendor) pluginvendor = vst3info->vendor;

			// make id or take it from vst3 info --------------
			std::string id(clapdescr->id);
			Crypto::uuid_object g;
			if (vst3info && vst3info->componentId)
			{
				memcpy(&g, vst3info->componentId, sizeof(g));
			}
			else
			{
				g = Crypto::create_sha1_guid_from_name(id.c_str(), id.size());
			}

			TUID lcid;			
			memcpy(&lcid, &g, sizeof(TUID));
			
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

			gCreationContexts.push_back({ &gClapLibrary, ctr, PClassInfo2(
				lcid,
				PClassInfo::kManyInstances,
				kVstAudioEffectClass,
				plugname,
				0		/* the only flag is usually Vst:kDistributable, but CLAPs aren't distributable */,
				features.c_str(),
				pluginvendor,
				clapdescr->version,
				kVstVersionString)
				});
			gPluginFactory->registerClass(&gCreationContexts.back().classinfo, ClapAsVst3::createInstance, &gCreationContexts.back());
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
	if (ctx->lib->hasEntryPoint())
	{
		return (IAudioProcessor*)new ClapAsVst3(ctx->lib, ctx->index, context);
	}
	return nullptr;	// this should never happen.
	
}
