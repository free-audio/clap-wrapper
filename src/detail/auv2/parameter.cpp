
#include "parameter.h"

namespace Clap::AUv2
{

Parameter::Parameter(const clap_plugin_t* plugin, const clap_plugin_params_t* clap_param_ext, const clap_param_info_t &clap_param)
{
  updateInfo(plugin, clap_param_ext, clap_param);
}

void Parameter::updateInfo(const clap_plugin_t* plugin, const clap_plugin_params_t* clap_param_ext, const clap_param_info_t& i)
{
  if ( _cfstring )
  {
    CFRelease(_cfstring);
  }
  _info = i;
  _cfstring = CFStringCreateWithCString(NULL, _info.name, kCFStringEncodingUTF8);
  
  const auto& info = _info;
  AudioUnitParameterOptions flags = 0;

  flags |= kAudioUnitParameterFlag_Global;

  if (!(info.flags & CLAP_PARAM_IS_AUTOMATABLE)) flags |= kAudioUnitParameterFlag_NonRealTime;
  if (!(info.flags & CLAP_PARAM_IS_HIDDEN))
  {
    if (info.flags & CLAP_PARAM_IS_READONLY)
      flags |= kAudioUnitParameterFlag_IsReadable;
    else
      flags |= kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable;
  }
  if (info.flags & CLAP_PARAM_IS_STEPPED)
  {
    if (info.max_value - info.min_value == 1)
    {
      flags |= kAudioUnitParameterUnit_Boolean;
    }
    // flags |= kAudioUnitParameterUnit_Indexed;  // probably need to add the lists then
  }
  if (info.max_value - info.min_value > 100)
  {
    flags |= kAudioUnitParameterFlag_IsHighResolution;
  }

  // checking if the parameter supports the conversion of its value to text

  // we can't get the value since we are not in the audio thread
  // auto guarantee_mainthread = _plugin->AlwaysMainThread();
  
  {
    char buf[200];
    if (clap_param_ext->value_to_text(plugin, info.id, info.default_value, buf, sizeof(buf)))
    {
      flags |= kAudioUnitParameterFlag_HasName;
    }
  }

  /*
   * The CFString() used from the param can reset which releases it. So add a ref count
   * and ask the param to release it too
   */
  flags |= kAudioUnitParameterFlag_HasCFNameString | kAudioUnitParameterFlag_CFNameRelease;
  
  _flags = flags;
}
Parameter::~Parameter()
{
  CFRelease(_cfstring);
}

}  // namespace Clap::AUv2
