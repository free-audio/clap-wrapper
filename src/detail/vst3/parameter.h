#pragma once

/*
    Vst3Parameter

    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    is being derived from Steinberg::Vst::Parameter and additionally stores
    information about the CLAP info (like the unique id of the CLAP parameter
    as well as the cookie pointer which is needed to address a parameter change
    properly).

    Still, the wrapper will use the ParameterContainer (std::vector<IPtr<Parameter>>)
    to communicate with the VST3 host.

    call Vst3Parameter::create(clapinfo) to create a heap based instance of it.

    The create function will apply everything necessary to the Vst::Parameter object in terms
    of value range conversion.

*/

#include <clap/ext/params.h>
#include <public.sdk/source/vst/vstparameters.h>
#include <functional>

class Vst3Parameter : public Steinberg::Vst::Parameter
{
  using super = Steinberg::Vst::Parameter;

 protected:
  Vst3Parameter(const Steinberg::Vst::ParameterInfo& vst3info, const clap_param_info_t* clapinfo);
  Vst3Parameter(const Steinberg::Vst::ParameterInfo& vst3info, uint8_t bus, uint8_t channel, uint8_t cc);

 public:
  virtual ~Vst3Parameter();
  bool setNormalized(Steinberg::Vst::ParamValue v) override;

#if 0
  // toString/fromString from VST3 parameter should actually be overloaded, but the wrapper will not call them
  // for the conversion, the _plugin instance is needed and we don't want to keep a copy in each parameter
  // the IEditController will be called anyway from the host, therefore the conversion will take place there.

  /** Converts a normalized value to a string. */
  void toString(Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string) const override;
  /** Converts a string to a normalized value. */
  bool fromString(const Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized) const override;
#endif

  inline double asClapValue(double vst3value) const
  {
    if (info.stepCount > 0)
    {
      return (vst3value * info.stepCount + 1) + min_value;
    }
    return (vst3value * (max_value - min_value)) + min_value;
  }
  inline double asVst3Value(double clapvalue) const
  {
    auto& info = this->getInfo();
    if (info.stepCount > 0)
    {
      return (clapvalue - min_value) / float(info.stepCount + 1);
    }
    return (clapvalue - min_value) / (max_value - min_value);
  }
  static Vst3Parameter* create(const clap_param_info_t* info,
                               std::function<Steinberg::Vst::UnitID(const char* modulepath)> getUnitId);
  static Vst3Parameter* create(uint8_t bus, uint8_t channel, uint8_t cc, Steinberg::Vst::ParamID id);
  // copies from the clap_param_info_t
  uint32_t param_index_for_clap_get_info = 0;
  clap_id id = 0;

  void* cookie = nullptr;
  double min_value;  // minimum plain value
  double max_value;  // maximum plain value
  // or it was MIDI
  bool isMidi = false;
  uint8_t channel = 0;
  uint8_t controller = 0;
};
