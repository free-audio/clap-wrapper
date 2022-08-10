#include "parameter.h"
#include <string>

using namespace Steinberg;

Vst3Parameter::Vst3Parameter(const Steinberg::Vst::ParameterInfo& vst3info, const clap_param_info_t* clapinfo)
: Steinberg::Vst::Parameter(vst3info)
, id(clapinfo->id)
, cookie(clapinfo->cookie)
, min_value(clapinfo->min_value)
, max_value(clapinfo->max_value)
{
  // 
}

Vst3Parameter::~Vst3Parameter()
{
}

Vst3Parameter* Vst3Parameter::create(const clap_param_info_t* info)
{
	Vst::ParameterInfo v;

	v.id = info->id & 0x7FFFFFFF;	// why ever SMTG does not want the highest bit to be set

	// the long name might contain the module name
	// this will change when we split the module to units
	std::string fullname(info->module);
	if (!fullname.empty())
	{
		fullname.append("/");
	}
	fullname.append(info->name);
	if (fullname.size() >= str16BufferSize(v.title))
	{
		fullname = info->name;
	}
	str8ToStr16(v.title, fullname.c_str(), str16BufferSize(v.title));
	// TODO: string shrink algorithm shortening the string a bit
	str8ToStr16(v.shortTitle, info->name, str16BufferSize(v.shortTitle));
	v.units[0] = 0;  // unfortunately, CLAP has no unit for parameter values
	v.unitId = 0;

	v.defaultNormalizedValue = info->default_value;
	v.flags = Vst::ParameterInfo::kNoFlags
		| ((info->flags & CLAP_PARAM_IS_HIDDEN) ? Vst::ParameterInfo::kIsHidden : 0)
		| ((info->flags & CLAP_PARAM_IS_BYPASS) ? Vst::ParameterInfo::kIsBypass : 0)
		| ((info->flags & CLAP_PARAM_IS_AUTOMATABLE) ? Vst::ParameterInfo::kCanAutomate : 0)
		| ((info->flags & CLAP_PARAM_IS_READONLY) ? Vst::ParameterInfo::kIsReadOnly : 0)
		| ((info->flags & CLAP_PARAM_IS_READONLY) ? Vst::ParameterInfo::kIsReadOnly : 0)

		;

	v.defaultNormalizedValue = info->default_value / info->max_value;
	if (info->flags & CLAP_PARAM_IS_STEPPED)
	{
		auto steps = (info->max_value - info->min_value) + 1;
		v.stepCount = steps;
	}
	else
		v.stepCount = 0;

	auto result = new Vst3Parameter(v, info);
	result->addRef();	// ParameterContainer doesn't add the ref -> but we don't have copies
	return result;
}
