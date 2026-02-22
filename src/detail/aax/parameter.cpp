#include "parameter.h"
#include "util.h"
#include "wrapper.h"

AAX_ClapParamDisplayDelegate::AAX_ClapParamDisplayDelegate(std::shared_ptr<AAXWrappedParameterInfo_t> info)
  : AAX_IDisplayDelegate<double>()
  ,_info(info)
{
  // yes, we have all we need
}
AAX_ClapParamDisplayDelegate* AAX_ClapParamDisplayDelegate::Clone() const
{
  return new AAX_ClapParamDisplayDelegate(*this);
}
bool		AAX_ClapParamDisplayDelegate::ValueToString(double value, AAX_CString* valueString) const
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

bool		AAX_ClapParamDisplayDelegate::ValueToString(double value, int32_t maxNumChars, AAX_CString* valueString) const
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
bool		AAX_ClapParamDisplayDelegate::StringToValue(const AAX_CString& valueString, double* value) const
{
  auto i = _info.get();
  return i->_ext_params->text_to_value(i->_plugin, i->_clap_param_info.id, valueString.Get(), value);
}


#if 0  // don't use that clutter
#include "AAX_CBinaryTaperDelegate.h"
#include "AAX_CLinearTaperDelegate.h"
#include "AAX_CStateTaperDelegate.h"
#include "AAX_CBinaryDisplayDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CStateDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"
#include "AAX_CDecibelDisplayDelegateDecorator.h"
#endif
// #include "AAX_UtilsNative.h"
// #include "AAX_MIDIUtilities.h"

#if 0
static inline char gen(uint32_t m)
{
  return "0123456789abcdef"[m & 0xF];
}

// creates a AAX string based id, which must not be larger than 32 characters.
static std::string createId(clap_id id)
{
  std::string result;
  uint32_t n = 32;
  while (n > 0)
  {
    n -= 4;
    result.push_back(gen(id >> n));
  }
  return result;
}
#endif

#if 0 
AAXWrappedParameterValue::AAXWrappedParameterValue(ClapAsAAX* plugin, const clap_param_info_t& ci)
  : _info(plugin, ci,  createAAXId(ci.id))
{
}

AAXWrappedParameter::AAXWrappedParameter(ClapAsAAX* plugin, const clap_param_info_t& clapinfo)
  : AAX_IParameter(), _value(plugin, clapinfo), _name(clapinfo.name)
{
  std::string paramname;

  if (clapinfo.module[0])
  {
    paramname = clapinfo.module;
    paramname.push_back('/');
  }
  paramname.append(clapinfo.name);

  auto n = generateShortStrings(paramname);
  _names.reserve(n.size());
  for (const auto& i : n)
  {
    _names.emplace_back(i.c_str());
  }
}

AAX_IParameterValue* AAXWrappedParameter::CloneValue() const {
  return _value.Clone();
}

AAX_CParamID AAXWrappedParameter::Identifier() const {
  return _value._info._identifier.c_str();
}

void AAXWrappedParameter::SetName(const AAX_CString& name)
{
  // we comply to the interface here, but actually the name shouldn't be changed
  // and the host shouldn't call this.
  _name = name;
}

const AAX_CString& AAXWrappedParameter::Name() const
{
  return _name;
}

void AAXWrappedParameter::AddShortenedName(const AAX_CString& name)
{
  _names.emplace_back(name);
}

const AAX_CString& AAXWrappedParameter::ShortenedName(int32_t iNumCharacters) const
{
  const AAX_CString* result = &_name;
  for (const auto& i : _names)
  {
    if (i.Length() > iNumCharacters)
      return *result;
    result = &i;
  }
  return *result;
}

void AAXWrappedParameter::ClearShortenedNames() {
  _names.clear();
}

bool AAXWrappedParameter::Automatable() const
{
  return _value._info._clap_param_info.flags & CLAP_PARAM_IS_AUTOMATABLE;
}

void AAXWrappedParameter::SetAutomationDelegate(AAX_IAutomationDelegate* iAutomationDelegate)
{
  //Remove the old automation delegate
  if (_aax_automationdelegate)
  {
    _aax_automationdelegate->UnregisterParameter(this->Identifier());
  }
  _aax_automationdelegate = iAutomationDelegate;
  if (_aax_automationdelegate)
  {
    _aax_automationdelegate->RegisterParameter(this->Identifier());
  }
}

void AAXWrappedParameter::Touch()
{
  if (_aax_automationdelegate)
    _aax_automationdelegate->PostTouchRequest(this->Identifier());
}

void AAXWrappedParameter::Release()
{
  if (_aax_automationdelegate)
    _aax_automationdelegate->PostReleaseRequest(this->Identifier());
}

// Taper Methods
// --------------------------------------------------------------------------------------------------

 void AAXWrappedParameter::SetNormalizedValue(double newNormalizedValue) {}
 double AAXWrappedParameter::GetNormalizedValue() const { return 0.0; }
 void AAXWrappedParameter::SetNormalizedDefaultValue(double normalizedDefault) {}
 double AAXWrappedParameter::GetNormalizedDefaultValue() const { return 0.0; }
 void AAXWrappedParameter::SetToDefaultValue() {}
 void AAXWrappedParameter::SetNumberOfSteps(uint32_t numSteps) {}
 uint32_t AAXWrappedParameter::GetNumberOfSteps() const { return 128; }
 uint32_t AAXWrappedParameter::GetStepValue() const { return 0; }
 double AAXWrappedParameter::GetNormalizedValueFromStep(uint32_t iStep) const {
   return 0.0f;
 }
 uint32_t AAXWrappedParameter::GetStepValueFromNormalizedValue(double normalizedValue) const { return 0; }
 void AAXWrappedParameter::SetStepValue(uint32_t iStep) {

 }


// Display Methods
 // --------------------------------------------------------------------------------------------------

 bool AAXWrappedParameter::AAXWrappedParameter::GetValueString(AAX_CString* valueString) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetValueString(int32_t iMaxNumChars, AAX_CString* valueString) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetNormalizedValueFromBool(bool value, double* normalizedValue) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetNormalizedValueFromInt32(int32_t value, double* normalizedValue) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetNormalizedValueFromFloat(float value, double* normalizedValue) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetNormalizedValueFromDouble(double value, double* normalizedValue) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetNormalizedValueFromString(const AAX_CString& valueString,
                                                        double* normalizedValue) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetBoolFromNormalizedValue(double normalizedValue, bool* value) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetInt32FromNormalizedValue(double normalizedValue, int32_t* value) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetFloatFromNormalizedValue(double normalizedValue, float* value) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetDoubleFromNormalizedValue(double normalizedValue, double* value) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetStringFromNormalizedValue(double normalizedValue,
                                                        AAX_CString& valueString) const
 {
   return false;
 }
 bool AAXWrappedParameter::GetStringFromNormalizedValue(double normalizedValue, int32_t iMaxNumChars,
                                                        AAX_CString& valueString) const
 {
   return false;
 }
 bool AAXWrappedParameter::SetValueFromString(const AAX_CString& newValueString)
 {
   return false;
 }

// Typed accessors 
// --------------------------------------------------------------------------------------------------

bool AAXWrappedParameter::GetValueAsBool(bool* value) const
{
  return _value.GetValueAsBool(value);
}

bool	AAXWrappedParameter::GetValueAsInt32(int32_t* value) const {
  return _value.GetValueAsInt32(value);
}

bool AAXWrappedParameter::GetValueAsFloat(float* value) const {
  return _value.GetValueAsFloat(value);
}

bool AAXWrappedParameter::GetValueAsDouble(double* value) const {
  return _value.GetValueAsDouble(value);
}

bool AAXWrappedParameter::GetValueAsString(AAX_IString* value) const {
  return _value.GetValueAsString(value);
}

bool AAXWrappedParameter::SetValueWithBool(bool value)
{  
  return false;
}

bool AAXWrappedParameter::SetValueWithInt32(int32_t value)
{
  return false;
}

bool AAXWrappedParameter::SetValueWithFloat(float value)
{
  return false;
}

bool AAXWrappedParameter::SetValueWithDouble(double value)
{
  return false;
}

bool AAXWrappedParameter::SetValueWithString(const AAX_IString& value)
{
  return false;
}

void AAXWrappedParameter::SetType(AAX_EParameterType iControlType)
{
}

AAX_EParameterType AAXWrappedParameter::GetType() const
{
  return AAX_EParameterType();
}

void AAXWrappedParameter::SetOrientation(AAX_EParameterOrientation iOrientation)
{
}

AAX_EParameterOrientation AAXWrappedParameter::GetOrientation() const
{
  return AAX_EParameterOrientation();
}
#endif  // AAXWrappedParameter

#if 0
AAX_IParameter* createParameter(const clap_param_info_t& cl)
{
  AAX_IParameter* result = nullptr;
  auto id_as_string = createId(cl.id);
  bool automatable = cl.flags & CLAP_PARAM_IS_AUTOMATABLE;
  if (cl.flags & CLAP_PARAM_IS_STEPPED)
  {
  auto info = std::make_unique<AAXWrappedParameterInfo_t>(cl, id_as_string);
  result = new AAXWrappedParameter<float>(
    info,
    AAX_CString(cl.name),
    cl.default_value,
    AAX_CBinaryTaperDelegate<float>(cl.min_value, cl.max_value),
    AAX_CDecibelDisplayDelegateDecorator<float>(AAX_CNumberDisplayDelegate<float, 1, 1>()),
    automatable);
    return result;
  }

  auto info = std::make_unique<AAXWrappedParameterInfo_t>(cl, id_as_string);
  result = new AAXWrappedParameter<float>(
    info,
    AAX_CString(cl.name),
    cl.default_value,
    AAX_CLinearTaperDelegate<float>(cl.min_value, cl.max_value),
    AAX_CDecibelDisplayDelegateDecorator<float>(AAX_CNumberDisplayDelegate<float, 1, 1>()),
    automatable);
  return result;
}
#endif
