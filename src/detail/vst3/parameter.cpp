#include "parameter.h"
#include <string>
#include <pluginterfaces/vst/ivstmidicontrollers.h>
#include <pluginterfaces/vst/ivstunits.h>

#if CLAP_VERSION_LT(1, 2, 0)
static_assert(false, "the CLAP-as-VST3 wrapper requires at least CLAP 1.2.0");
/*
*   CLAP_PARAM_IS_ENUM is available with CLAP 1.2.0
*
*   This is only a requirement to compile the wrapper properly, it will still wrap CLAPs compiled with earlier versions of CLAP.
*/
#endif

using namespace Steinberg;

Vst3Parameter::Vst3Parameter(const Steinberg::Vst::ParameterInfo& vst3info,
                             const clap_param_info_t* clapinfo)
  : Steinberg::Vst::Parameter(vst3info)
  , id(clapinfo->id)
  , cookie(clapinfo->cookie)
  , min_value(clapinfo->min_value)
  , max_value(clapinfo->max_value)
{
  //
}

Vst3Parameter::Vst3Parameter(const Steinberg::Vst::ParameterInfo& vst3info, uint8_t /*bus*/,
                             uint8_t channel, uint8_t cc)
  : Steinberg::Vst::Parameter(vst3info)
  , id(vst3info.id)
  , cookie(nullptr)
  , min_value(0)
  , max_value(127)
  , isMidi(true)
  , channel(channel)
  , controller(cc)
{
  if (cc == Vst::ControllerNumbers::kPitchBend)
  {
    max_value = 16383;
  }
}
Vst3Parameter::~Vst3Parameter() = default;

bool Vst3Parameter::setNormalized(Steinberg::Vst::ParamValue v)
{
  if (isMidi && (info.flags & Steinberg::Vst::ParameterInfo::kIsProgramChange))
  {
    return true;
  }
  return super::setNormalized(v);
}

#if 0
void Vst3Parameter::toString(Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string) const
{	
	super::toString(valueNormalized, string);
}
/** Converts a string to a normalized value. */
bool Vst3Parameter::fromString(const Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized) const
{
	return super::fromString(string, valueNormalized);
}
#endif

Vst3Parameter* Vst3Parameter::create(
    const clap_param_info_t* info,
    std::function<Steinberg::Vst::UnitID(const char* modulepath)> getUnitId)
{
  Vst::ParameterInfo v;

  v.id = info->id & 0x7FFFFFFF;  // why ever SMTG does not want the highest bit to be set

  // the long name might contain the module name
  // this will change when we split the module to units
  std::string fullname;
  Vst::UnitID unit = 0;

  // if there is a module name and a lambda
  if (info->module[0] != 0 && getUnitId)
  {
    unit = getUnitId(info->module);
    fullname = info->name;
  }
  else
  {
    if (!fullname.empty())
    {
      fullname.append("/");
    }
    fullname.append(info->name);
    if (fullname.size() >= str16BufferSize(v.title))
    {
      fullname = info->name;
    }
  }

  str8ToStr16(v.title, fullname.c_str(), str16BufferSize(v.title));
  // TODO: string shrink algorithm shortening the string a bit
  str8ToStr16(v.shortTitle, info->name, str16BufferSize(v.shortTitle));
  v.units[0] = 0;  // unfortunately, CLAP has no unit for parameter values
  v.unitId = unit;

  /*
			In the VST3 SDK the normalized value [0, 1] to discrete value and its inverse function discrete value to normalized value is defined like this:

			Normalize:
			double normalized = discreteValue / (double) stepCount;

			Denormalize :
			int discreteValue = min (stepCount, normalized * (stepCount + 1));
	*/

  v.flags = Vst::ParameterInfo::kNoFlags |
            ((info->flags & CLAP_PARAM_IS_HIDDEN) ? Vst::ParameterInfo::kIsHidden : 0) |
            ((info->flags & CLAP_PARAM_IS_BYPASS) ? Vst::ParameterInfo::kIsBypass : 0) |
            ((info->flags & CLAP_PARAM_IS_AUTOMATABLE) ? Vst::ParameterInfo::kCanAutomate : 0) |
            ((info->flags & CLAP_PARAM_IS_READONLY) ? Vst::ParameterInfo::kIsReadOnly : 0) |
            ((info->flags & CLAP_PARAM_IS_ENUM) ? Vst::ParameterInfo::kIsList : 0)

      // | ((info->flags & CLAP_PARAM_IS_READONLY) ? Vst::ParameterInfo::kIsReadOnly : 0)
      ;

  auto param_range = (info->max_value - info->min_value);

  v.defaultNormalizedValue = (info->default_value - info->min_value) / param_range;
  if ((info->flags & CLAP_PARAM_IS_STEPPED) || (info->flags & CLAP_PARAM_IS_ENUM))
  {
    auto steps = param_range + 1;
    v.stepCount = steps;
  }
  else
    v.stepCount = 0;

  auto result = new Vst3Parameter(v, info);
  result->addRef();  // ParameterContainer doesn't add the ref -> but we don't have copies
  return result;
}

Vst3Parameter* Vst3Parameter::create(uint8_t bus, uint8_t channel, uint8_t cc, Vst::ParamID id)
{
  Vst::ParameterInfo v;

  v.id = id;

  auto name = "controller";
  // the long name might contain the module name
  // this will change when we split the module to units
  std::string fullname("MIDI");
  if (!fullname.empty())
  {
    fullname.append("/");
  }
  fullname.append("controller");
  if (fullname.size() >= str16BufferSize(v.title))
  {
    fullname = "controller";
  }
  str8ToStr16(v.title, fullname.c_str(), str16BufferSize(v.title));
  // TODO: string shrink algorithm shortening the string a bit
  str8ToStr16(v.shortTitle, name, str16BufferSize(v.shortTitle));

  v.units[0] = 0;  // nothing in the "unit" field
  // the unit will not be set here, but outside
  // v.unitId = channel + 1;

  v.defaultNormalizedValue = 0;
  v.flags = Vst::ParameterInfo::kNoFlags;
  if (cc == Vst::ControllerNumbers::kCtrlProgramChange)
  {
    v.flags |= Vst::ParameterInfo::kIsProgramChange | Vst::ParameterInfo::kCanAutomate;
    v.stepCount = 128;
  }

  v.defaultNormalizedValue = 0;
  v.stepCount = 128;

  if (cc == Vst::ControllerNumbers::kPitchBend)
  {
    v.stepCount = 16384;
  }

  auto result = new Vst3Parameter(v, bus, channel, cc);
  result->addRef();  // ParameterContainer doesn't add the ref -> but we don't have copies
  return result;
}
