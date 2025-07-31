#pragma once

/*
    AUv2 process Adapter
 
    Copyright (c) 2023 Timo Kaluza (defiantnerd)
 
    This file i spart of the clap-wrappers project which is released under MIT License
 
 See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.
 
 
 The process adapter is responsible to translate events, timing information
 
 */

#include <clap/clap.h>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wextra"
#endif

#include <AudioUnitSDK/AUBase.h>

// TODO: check if additional AU headers are needed
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnitParameters.h>
#include <AudioUnit/AUComponent.h>


namespace Clap::AUv2
{

class Parameter
{
 public:
  Parameter(const clap_plugin_t* plugin, const clap_plugin_params_t* clap_param_ext, const clap_param_info_t& clap_param);
  ~Parameter();
  const clap_param_info_t& info() const
  {
    return _info;
  }
  const CFStringRef CFString() const
  {
    return _cfstring;
  }
  AudioUnitParameterOptions AudioUnitFlags() const
  {
    return _flags;
  }

  void updateInfo(const clap_plugin_t* plugin, const clap_plugin_params_t* clap_param_ext, const clap_param_info_t& i);

 private:
  clap_param_info_t _info;
  CFStringRef _cfstring = nullptr;
  AudioUnitParameterOptions _flags;
};

}  // namespace Clap::AUv2
