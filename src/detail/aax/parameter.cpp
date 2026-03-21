#include "parameter.h"
#include "util.h"
#include "wrapper.h"

AAX_ClapParamDisplayDelegate::AAX_ClapParamDisplayDelegate(
    std::shared_ptr<AAXWrappedParameterInfo_t> info)
  : AAX_IDisplayDelegate<double>(), _info(info)
{
  // yes, we have all we need
}
AAX_ClapParamDisplayDelegate *AAX_ClapParamDisplayDelegate::Clone() const
{
  return new AAX_ClapParamDisplayDelegate(*this);
}
bool AAX_ClapParamDisplayDelegate::ValueToString(double value, AAX_CString *valueString) const
{
  auto i = _info.get();
  char buf[101];
  if (i->_ext_params->value_to_text(i->_plugin, i->_clap_param_info.id, i->asClapValue(value), buf, 100))
  {
    valueString->Set(buf);
    return true;
  }
  return false;
}

bool AAX_ClapParamDisplayDelegate::ValueToString(double value, int32_t maxNumChars,
                                                 AAX_CString *valueString) const
{
  auto i = _info.get();
  char buf[101];
  int32_t sz = 100;
  if (maxNumChars < 100) sz = maxNumChars;
  if (i->_ext_params->value_to_text(i->_plugin, i->_clap_param_info.id, i->asClapValue(value), buf, sz))
  {
    valueString->Set(buf);
    return true;
  }
  return false;
}
bool AAX_ClapParamDisplayDelegate::StringToValue(const AAX_CString &valueString, double *value) const
{
  auto i = _info.get();
  return i->_ext_params->text_to_value(i->_plugin, i->_clap_param_info.id, valueString.Get(), value);
}
