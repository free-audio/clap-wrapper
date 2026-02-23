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

// TODO: REmove
#include "AAX_CLinearTaperDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"

class ClapAsAAX;

typedef struct AAXWrappedParameterInfo
{
  AAXWrappedParameterInfo(const clap_plugin_t* plugin, const clap_param_info_t& ci,
                          const std::string identifier)
    : _plugin(plugin), _clap_param_info(ci), _aax_identifier(identifier)
  {
  }
  const clap_plugin_t* _plugin;
  const clap_plugin_params_t* _ext_params = nullptr;
  clap_param_info_t _clap_param_info;
  std::string _aax_identifier;  // someone has too keep the buffer
  std::vector<AAX_CString> _names;
  int32_t _paramAAXIndex;

  inline bool isAutomatable() const
  {
    return _clap_param_info.flags & CLAP_PARAM_IS_AUTOMATABLE;
  }
  inline double asClapValue(double aaxvalue) const
  {
    if (_clap_param_info.flags & CLAP_PARAM_IS_STEPPED)
    {
      return (aaxvalue * (_clap_param_info.max_value - _clap_param_info.min_value)) +
             _clap_param_info.min_value;
    }
    return (aaxvalue * (_clap_param_info.max_value - _clap_param_info.min_value)) +
           _clap_param_info.min_value;
  }
  inline double asAAXValue(double clapvalue) const
  {
    if (_clap_param_info.flags & CLAP_PARAM_IS_STEPPED)
    {
      return floor(clapvalue - _clap_param_info.min_value) /
             (_clap_param_info.max_value - _clap_param_info.min_value);
    }
    return (clapvalue - _clap_param_info.min_value) /
           (_clap_param_info.max_value - _clap_param_info.min_value);
  }

} AAXWrappedParameterInfo_t;

// we only know doubles:
// the AAX_ClapParamDisplayDelegate will allow the translation between doubles and text values.
class AAX_ClapParamDisplayDelegate : public AAX_IDisplayDelegate<double>
{
 public:
  //Virtual Overrides
  AAX_ClapParamDisplayDelegate(std::shared_ptr<AAXWrappedParameterInfo_t> info);
  AAX_ClapParamDisplayDelegate* Clone() const AAX_OVERRIDE;
  bool ValueToString(double value, AAX_CString* valueString) const AAX_OVERRIDE;
  bool ValueToString(double value, int32_t maxNumChars, AAX_CString* valueString) const AAX_OVERRIDE;
  bool StringToValue(const AAX_CString& valueString, double* value) const AAX_OVERRIDE;

 protected:
  std::shared_ptr<AAXWrappedParameterInfo_t> _info;
};

#if 0 
class AAXWrappedParameter;

class AAXWrappedParameterValue : public AAX_IParameterValue
{
public:
  friend class AAXWrappedParameter;
  AAXWrappedParameterValue(ClapAsAAX* plugin, const clap_param_info_t& ci);
  ~AAXWrappedParameterValue() override {}
  AAX_IParameterValue* Clone() const override { return new AAXWrappedParameterValue(_info._plugin, _info._clap_param_info); }
  AAX_CParamID				Identifier() const override { return _info._identifier.c_str(); }
  bool		GetValueAsBool(bool* value) const override { return false; }
  bool		GetValueAsInt32(int32_t* value) const override { return false; }
  bool		GetValueAsFloat(float* value) const override { return false; }
  bool		GetValueAsDouble(double* value) const override { return false; }
  bool		GetValueAsString(AAX_IString* value) const override { return false; }

protected:
  AAXWrappedParameterInfo_t _info;
};

class AAXWrappedParameter : public AAX_IParameter
{
public:
  AAXWrappedParameter(ClapAsAAX* plugin, const clap_param_info_t& clapinfo);
 //s td::unique_ptr<AAXWrappedParameterInfo_t>& info, const AAX_IString& name, T defaultValue, const AAX_ITaperDelegate<T>& taperDelegate, const AAX_IDisplayDelegate<T>& displayDelegate, bool automatable)
 //  : AAX_CParameter(info->_identifier.c_str(), name, defaultValue, taperDelegate, displayDelegate, automatable)
 //  , _info(std::move(info))
 //{
 //}
 virtual ~AAXWrappedParameter()
 {
 }
 AAX_IParameterValue* CloneValue() const override;
 AAX_CParamID Identifier() const override;

 // Identification methods

 void SetName(const AAX_CString& name) override;
 const AAX_CString& Name() const override;
 void AddShortenedName(const AAX_CString& name)  override;
 const AAX_CString& ShortenedName(int32_t iNumCharacters) const override;
 void ClearShortenedNames() override;

 // automation methods

 bool Automatable() const override;
 void SetAutomationDelegate(AAX_IAutomationDelegate* iAutomationDelegate) override;
 void Touch() override;
 void Release() override;

 // Taper methods

 void SetNormalizedValue(double newNormalizedValue) override;
 double GetNormalizedValue() const override;
 void SetNormalizedDefaultValue(double normalizedDefault) override;
 double GetNormalizedDefaultValue() const override;
 void SetToDefaultValue() override;
 void SetNumberOfSteps(uint32_t numSteps) override;
 uint32_t GetNumberOfSteps() const override;
 uint32_t GetStepValue() const override;
 double GetNormalizedValueFromStep(uint32_t iStep) const override;
 uint32_t GetStepValueFromNormalizedValue(double normalizedValue) const override;
 void SetStepValue(uint32_t iStep) override;

 // Display methods

 bool GetValueString(AAX_CString* valueString) const override;
 bool GetValueString(int32_t iMaxNumChars, AAX_CString* valueString) const override;
 bool GetNormalizedValueFromBool(bool value, double* normalizedValue) const override;
 bool GetNormalizedValueFromInt32(int32_t value, double* normalizedValue) const override;
 bool GetNormalizedValueFromFloat(float value, double* normalizedValue) const override;
 bool GetNormalizedValueFromDouble(double value, double* normalizedValue) const override;
 bool GetNormalizedValueFromString(const AAX_CString& valueString,
                                   double* normalizedValue) const override;
 bool GetBoolFromNormalizedValue(double normalizedValue, bool* value) const override;
 bool GetInt32FromNormalizedValue(double normalizedValue, int32_t* value) const override;
 bool GetFloatFromNormalizedValue(double normalizedValue, float* value) const override;
 bool GetDoubleFromNormalizedValue(double normalizedValue, double* value) const override;
 bool GetStringFromNormalizedValue(double normalizedValue, AAX_CString& valueString) const override;
 bool GetStringFromNormalizedValue(double normalizedValue, int32_t iMaxNumChars,
                                   AAX_CString& valueString) const override;
 bool SetValueFromString(const AAX_CString& newValueString) override;

 // Typed accessors

 bool GetValueAsBool(bool* value) const override;
 bool GetValueAsInt32(int32_t* value) const override;
 bool GetValueAsFloat(float* value) const override;
 bool GetValueAsDouble(double* value) const override;
 bool GetValueAsString(AAX_IString* value) const override;

 bool SetValueWithBool(bool value) override;
 bool SetValueWithInt32(int32_t value) override;
 bool SetValueWithFloat(float value) override;
 bool SetValueWithDouble(double value) override;
 bool SetValueWithString(const AAX_IString& value) override;

 void SetType(AAX_EParameterType iControlType) override;
 AAX_EParameterType GetType() const override;
 void SetOrientation(AAX_EParameterOrientation iOrientation) override;
 AAX_EParameterOrientation GetOrientation() const override;

private:
 AAXWrappedParameterValue _value;
 // unfortunately, this is necessary to comply the IAAXParameter interface Name() method.
 AAX_CString _name;
 std::vector<AAX_CString> _names;
 AAX_IAutomationDelegate* _aax_automationdelegate = nullptr;
};

#endif