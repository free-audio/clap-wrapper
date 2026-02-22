// AAX entry points - the base AAX SDK library we've built is missing the AAX_Init.cpp by purpose.
// 
// this defines the DLL entry point that is actually provided by the AAX SDK in the AAX_Init.cpp
// We need some hooks in there, so we redefine the function names and provide our own versions
// but include the init code from the SDK

// ---------------8<-----------------
// step one: define our function declarations
#include "AAX.h"

#define AAXStartup AAXStartup_Base
#define AAXShutdown AAXShutdown_Base
#define AAXRegisterPlugin AAXRegisterPlugin_Base

#include "AAXLibrary/source/AAX_Init.cpp"

#undef AAXRegisterPlugin
#undef AAXShutdown
#undef AAXStartup
// ---------------8<-----------------
#include "factory.h"

// now we can hook in and locate the CLAP library and its entry point

AAX_Result AAXRegisterPlugin(IACFUnknown* pUnkHost, IACFPluginDefinition** ppPluginDefinition)
{
  return AAXRegisterPlugin_Base(pUnkHost, ppPluginDefinition);
}

AAX_Result AAXStartup(IACFUnknown* pUnkHost)
{
  // load our clap or return error
	os::log(os::getBinaryName());
	auto factory = CLAPAAX::guarantee_clap();
	if (!factory)
	{
		os::log("CLAP as AAX: plugin not found");
		
		return AAX_ERROR_NO_COMPONENTS;
	}
	os::log("CLAP as AAX: plugin found");
  return AAXStartup_Base(pUnkHost);
}

AAX_Result AAXShutdown(IACFUnknown* pUnkHost)
{
  // unload our clap
  return AAXShutdown_Base(pUnkHost);
}

#if WIN32
// ------------------------------------------------------------------------------------------------
// on Windows, we pass the iInstance and initialize our minimal os layer
// in a combined plugin that exports all flavors sametime, this needs to be refactored and abstracted

HINSTANCE ghInst = 0;

extern "C" BOOL WINAPI DllMain ( HINSTANCE iInstance, DWORD iSelector, LPVOID iReserved )
{
	try
	{
		if ( iSelector == DLL_PROCESS_ATTACH )
		{
			std::string ll("DllMain PROCESS_ATTACH: "); ll += os::getModulePath(); OutputDebugStringA(ll.c_str());
			ghInst = iInstance;
			os::init();
		}	
		if (iSelector == DLL_PROCESS_DETACH)
		{
			std::string ll("DllMain PROCESS_DETACH: "); ll += os::getModulePath(); OutputDebugStringA(ll.c_str());
			os::terminate();
		}
	}
	catch(std::exception& e)
	{
		std::string ll("Exception occured in DllMain: "); ll += e.what(); OutputDebugStringA(ll.c_str());
		return false;
	}

	return true;
}
// ------------------------------------------------------------------------------------------------
#endif
