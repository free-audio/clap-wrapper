
#include "public.sdk/source/main/pluginfactory.h"

SMTG_EXPORT_SYMBOL Steinberg::IPluginFactory *PLUGIN_API GetPluginFactory()
{
  extern Steinberg::IPluginFactory *GetPluginFactoryEntryPoint();
  return GetPluginFactoryEntryPoint();
}
