//------------------------------------------------------------------------
// Project     : VST SDK
//
// Category    : Examples
// Filename    : public.sdk/samples/vst/again/source/againentry.cpp
// Created by  : Steinberg, 04/2005
// Description : AGain Example for VST 3
//
//-----------------------------------------------------------------------------
// LICENSE
// (c) 2022, Steinberg Media Technologies GmbH, All Rights Reserved
//-----------------------------------------------------------------------------
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
// 
//   * Redistributions of source code must retain the above copyright notice, 
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation 
//     and/or other materials provided with the distribution.
//   * Neither the name of the Steinberg Media Technologies nor the names of its
//     contributors may be used to endorse or promote products derived from this 
//     software without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
// IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, 
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF 
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE 
// OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//-----------------------------------------------------------------------------

//#include "again.h"	// for AGain
//#include "againsidechain.h"	// for AGain SideChain
//#include "againcontroller.h" // for AGainController
//#include "againcids.h"	// for class ids and categrory
//#include "version.h"	// for versioning
#include "detail/sha1.h"
#include "wrapasvst3.h"
#include "wrapasvst3_version.h"
#include "public.sdk/source/main/pluginfactory.h"


// #define stringPluginName "CLAP as VST3"
// #define stringPluginSideChainName "AGain SideChain VST3"

#if TARGET_OS_IPHONE
#include "public.sdk/source/vst/vstguieditor.h"
extern void* moduleHandle;
#endif

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
	size_t index = 0;
	PClassInfo2 classinfo;
};

SMTG_EXPORT_SYMBOL IPluginFactory* PLUGIN_API GetPluginFactory() {

#if SMTG_OS_WINDOWS
// #pragma comment(linker, "/EXPORT:" __FUNCTION__ "=" __FUNCDNAME__)
#endif

	static IPtr<Steinberg::CPluginFactory> gPluginFactory = nullptr;
	static Clap::Library gClapLibrary;

	static std::vector<CreationContext> gCreationContexts;
	if (!gClapLibrary._pluginFactory)
	{
		if (!gClapLibrary.hasEntryPoint())
		{
			auto paths = Clap::getValidCLAPSearchPaths();
			for (auto& i : paths)
			{
				 auto k = i / "clap-saw-demo.clap";
				// auto k = i / "u-he" / "Diva.clap";
				// auto k = i / "Audiority" / "Space Station UM282.clap";
				if (gClapLibrary.load(k.u8string().c_str()))
				{
					break;
				}
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

	// we need at least one plugin to obtain vendor/name etc.
	auto* vendor = gClapLibrary.plugins[0]->vendor;
	auto* vendor_url = gClapLibrary.plugins[0]->url;
		
	// TODO: extract the domain and prefix with info@
	auto* contact = "info@";
	
	if (!gPluginFactory)
	{
		static PFactoryInfo factoryInfo(
			vendor, 
			vendor_url, 
			contact,
			Vst::kDefaultFactoryFlags);

		gPluginFactory = new Steinberg::CPluginFactory(factoryInfo);		
		// resize the classInfo vector
		gCreationContexts.reserve(gClapLibrary.plugins.size());
		for (size_t ctr = 0; ctr < gClapLibrary.plugins.size(); ++ctr)
		{
			auto& clapdescr = gClapLibrary.plugins[ctr];
			std::string n(clapdescr->name);
			n.append(" (CLAPWRAP)");
			auto plugname = n.c_str(); //  clapdescr->name;
			auto vendor = clapdescr->vendor;
			if (vendor == nullptr || *vendor == 0)
			{
				vendor = "Unspecified Vendor";
			}
			std::string id(clapdescr->id);
			auto g = Crypto::create_sha1_guid_from_name(id.c_str(), id.size());

			TUID lcid;			
			memcpy(&lcid, &g, sizeof(TUID));
			
			gCreationContexts.push_back({ &gClapLibrary, ctr, PClassInfo2(
				lcid,
				PClassInfo::kManyInstances,
				kVstAudioEffectClass,
				plugname,
				0		/* the only flag is usually Vst:kDistributable, but CLAPs aren't distributable */,
				clapCategoriesToVST3(clapdescr->features).c_str(),
				vendor,
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
*/
FUnknown* ClapAsVst3::createInstance(void* context)
{
	auto ctx = static_cast<CreationContext*>(context);
	if (!ctx->lib->hasEntryPoint())
	{
		auto paths = Clap::getValidCLAPSearchPaths();
		for (auto& i : paths)
		{
			auto k = i / "clap-saw-demo.clap";
			// auto k = i / "u-he" / "Diva.clap";
			if (ctx->lib->load(k.u8string().c_str()))
			{
				break;
			}
		}
	}
	return (IAudioProcessor*)new ClapAsVst3(ctx->lib, ctx->index, context);
	
}
