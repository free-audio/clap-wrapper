#pragma once

#include <AAX.h>
#include <AAX_IParameter.h>
#include "AAX_IAutomationDelegate.h"
#include <AAX_CString.h>
// #include <AAX_CParameter.h>
#include <clap/clap.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

// TODO: REmove
#include "AAX_CLinearTaperDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"

class ClapAsAAX;

typedef struct AAXWrappedParameterInfo
{
  AAXWrappedParameterInfo(const clap_plugin_t *plugin, const clap_param_info_t &ci,
                          const std::string identifier)
    : _plugin(plugin), _clap_param_info(ci), _aax_identifier(identifier)
  {
  }
  const clap_plugin_t *_plugin;
  const clap_plugin_params_t *_ext_params = nullptr;
  clap_param_info_t _clap_param_info;
  std::string _aax_identifier;  // someone has to keep the buffer
  std::vector<AAX_CString> _names;
  int32_t _paramAAXIndex = 0;

  inline bool isAutomatable() const
  {
    return _clap_param_info.flags & CLAP_PARAM_IS_AUTOMATABLE;
  }
  inline bool isStepped() const
  {
    return (_clap_param_info.flags & CLAP_PARAM_IS_STEPPED) != 0;
  }

  // AAX hands us a normalized [0, 1]; CLAP wants a value in [min_value, max_value].
  // A stepped parameter is discrete in CLAP, so the result is rounded to the nearest
  // step rather than handed over as the fraction AAX happened to land on.
  inline double asClapValue(double aaxvalue) const
  {
    const double range = _clap_param_info.max_value - _clap_param_info.min_value;
    double v = (aaxvalue * range) + _clap_param_info.min_value;
    if (isStepped()) v = std::round(v);
    return std::clamp(v, _clap_param_info.min_value, _clap_param_info.max_value);
  }

  // The inverse of asClapValue(). It rounds a stepped value the same way, so that
  // converting back and forth settles instead of drifting: without the matching
  // round, a value quantized on the way in would normalize to a different point
  // than the one AAX is holding.
  inline double asAAXValue(double clapvalue) const
  {
    const double range = _clap_param_info.max_value - _clap_param_info.min_value;
    if (range <= 0.0) return 0.0;  // degenerate range: never divide by it
    double v = clapvalue;
    if (isStepped()) v = std::round(v);
    return std::clamp((v - _clap_param_info.min_value) / range, 0.0, 1.0);
  }

} AAXWrappedParameterInfo_t;

// we only know doubles:
// the AAX_ClapParamDisplayDelegate will allow the translation between doubles and text values.
class AAX_ClapParamDisplayDelegate : public AAX_IDisplayDelegate<double>
{
 public:
  //Virtual Overrides
  AAX_ClapParamDisplayDelegate(std::shared_ptr<AAXWrappedParameterInfo_t> info);
  AAX_ClapParamDisplayDelegate *Clone() const AAX_OVERRIDE;
  bool ValueToString(double value, AAX_CString *valueString) const AAX_OVERRIDE;
  bool ValueToString(double value, int32_t maxNumChars, AAX_CString *valueString) const AAX_OVERRIDE;
  bool StringToValue(const AAX_CString &valueString, double *value) const AAX_OVERRIDE;

 protected:
  std::shared_ptr<AAXWrappedParameterInfo_t> _info;
};
