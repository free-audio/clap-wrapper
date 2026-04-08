/*
    CLAP as AAX

    Copyright (c) 2024-2026 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.
    
    This AAX opens a CLAP plugin and matches all corresponding AAX calls to it.
    For the AAX Host it is a AAX plugin, for the CLAP plugin it is a CLAP host.

*/

#include "wrapper.h"
// #include "clap_proxy.h"
#include "AAX.h"
#include "AAX_ICollection.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IEffectDescriptor.h"
#include "AAX_IPropertyMap.h"
#include "AAX_Exception.h"
#include "AAX_Errors.h"
#include "AAX_Assert.h"
#include "AAX_Init.h"
// #include <Topology/AAX_CMonolithicParameters.h> <- this introduces way too much clutter

// ----[CLAP]-----------------------------------------------------------------------

#include "factory.h"

// ---------------------------------------------------------------------------------
#include "detail/shared/util.h"

// ----[AAX WRAPPER]----------------------------------------------------------------
#include "process.h"
#include "categories.h"
#include "util.h"
#include "clapwrapper/aax.h"
#include "plugview.h"
#include "detail/os/osutil.h"

// ---------------------------------------------------------------------------------
#include <iostream>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unordered_set>

#include "audioconfig.h"

// #include <mutex>
// #include <functional>

class ClapAsAAXRegistry
{
 public:
  static void Register(ClapAsAAX *instance)
  {
    std::lock_guard<std::mutex> lock(GetMutex());
    GetSet().insert(instance);
  }
  static void Unregister(ClapAsAAX *instance)
  {
    std::lock_guard<std::mutex> lock(GetMutex());
    GetSet().erase(instance);
  }
  static void ForEach(const std::function<void(ClapAsAAX *)> &fn)
  {
    std::lock_guard<std::mutex> lock(GetMutex());
    for (auto *inst : GetSet())
    {
      fn(inst);
    }
  }
  static bool Exists(ClapAsAAX *instance)
  {
    std::lock_guard<std::mutex> lock(GetMutex());
    return GetSet().find(instance) != GetSet().end();
  }

 private:
  static std::unordered_set<ClapAsAAX *> &GetSet()
  {
    static std::unordered_set<ClapAsAAX *> set;
    return set;
  }
  static std::mutex &GetMutex()
  {
    static std::mutex mtx;
    return mtx;
  }
};

int32_t AAX_CALLBACK
AAXWrapper_inInstanceInitProc(const SAAX_Wrapper_AlgorithmicContext *inInstanceContextPtr,
                              AAX_EComponentInstanceInitAction inAction)
{
  auto self = inInstanceContextPtr->mPrivateData->wrapper;
  switch (inAction)
  {
    case AAX_eComponentInstanceInitAction_AddingNewInstance:
      LOGDETAIL("adding new instance");
      self->activatePlugin();
      self->startProcessing();
      break;
    case AAX_eComponentInstanceInitAction_RemovingInstance:
      LOGDETAIL("removing instance");
      if (ClapAsAAXRegistry::Exists(self))
      {
        self->stopProcessing();
        self->deactivatePlugin();
      }
      else
      {
        LOGDETAIL("instance to be removed does not exists anymore");
      }
      break;
    case AAX_eComponentInstanceInitAction_ResetInstance:
      LOGDETAIL("resetting instance!?");
      break;
    default:
      break;
  }
  return AAX_SUCCESS;
}

int32_t AAX_CALLBACK AAXWrapper_BackgroundProc()
{
  ClapAsAAXRegistry::ForEach([](ClapAsAAX *instance) { instance->onIdle(); });
  return AAX_SUCCESS;
}

// ---------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------------------

// AAX needs all the description for a component in advance - there is no dynamic thing in here.
static void DescribeAlgorithmComponent(AAX_IComponentDescriptor *outDesc,
                                       const Clap::Library *clapFactory, uint32_t plugindex,
                                       const clap_plugin_info_as_aax_t *aax_plugin_info,
                                       const CLAPAAX::plugin_bus_info_t &businfo,
                                       const CLAPAAX::stemformat_combi_t &stemformat)
{
  AAX_CheckedResult err;

  const clap_plugin_descriptor_t *clapDescriptor = clapFactory->plugins[plugindex];

  // Describe algorithm's context structure
  //
  // Add outputs, meters, info, etc
  err = outDesc->AddAudioIn(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mAudioInputs));
  err = outDesc->AddAudioOut(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mAudioOutputs));
  err = outDesc->AddAudioBufferLength(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mNumSamples));
  err = outDesc->AddClock(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mClock));

  // err = outDesc->AddMeters( AAX_FIELD_INDEX (SAAX_Wrapper_AlgorithmicContext, mMeters), setupInfo.mMeterIDs, static_cast<uint32_t>(setupInfo.mNumMeters) );
  err = outDesc->AddPrivateData(
      AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mMeters), sizeof(float),
      AAX_ePrivateDataOptions_DefaultOptions);  //Just here to fill the port.  Not used.

  // Register MIDI nodes. To avoid context corruption, register small blocks of private data for fields where a node is not needed
  AAX_CFieldIndex globalNodeID = AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mGlobalNode);
  AAX_CFieldIndex localInputNodeID = AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mInputNode);
  AAX_CFieldIndex transportNodeID = AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mTransportNode);

  // Global MIDI node — not currently used
  err = outDesc->AddPrivateData(globalNodeID, sizeof(float), AAX_ePrivateDataOptions_DefaultOptions);

  // Local MIDI input node
  if (businfo.has_midi_in)
  {
    if (aax_plugin_info && aax_plugin_info->midi_in_name)
      err = outDesc->AddMIDINode(localInputNodeID, AAX_eMIDINodeType_LocalInput,
                                 aax_plugin_info->midi_in_name, aax_plugin_info->midi_in_channel_mask);
    else
      err = outDesc->AddMIDINode(localInputNodeID, AAX_eMIDINodeType_LocalInput,
                                 businfo.midi_in_name.c_str(), 0xFFFF);
  }
  else
  {
    err =
        outDesc->AddPrivateData(localInputNodeID, sizeof(float), AAX_ePrivateDataOptions_DefaultOptions);
  }

  // Local MIDI output node
  if (businfo.has_midi_out)
  {
    if (aax_plugin_info && aax_plugin_info->midi_out_name)
      err = outDesc->AddMIDINode(localInputNodeID, AAX_eMIDINodeType_LocalOutput,
                                 aax_plugin_info->midi_out_name, aax_plugin_info->midi_out_channel_mask);
    else
      err = outDesc->AddMIDINode(localInputNodeID, AAX_eMIDINodeType_LocalOutput,
                                 businfo.midi_out_name.c_str(), 0xFFFF);
  }

  if (true)  // setupInfo.mNeedsTransport)
    err = outDesc->AddMIDINode(transportNodeID, AAX_eMIDINodeType_Transport, "Transport", 0xffff);
  else
    err = outDesc->AddPrivateData(
        transportNodeID, sizeof(float),
        AAX_ePrivateDataOptions_DefaultOptions);  //Just here to fill the port.  Not used.

  //Add pointer to the data model instance and other interesting information.
  err =
      outDesc->AddPrivateData(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mPrivateData),
                              sizeof(SAAX_Wrapper_PrivateData), AAX_ePrivateDataOptions_DefaultOptions);

  //Add a "state number" counter for deferred parameter updates
  err = outDesc->AddDataInPort(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mCurrentStateNum),
                               sizeof(uint64_t));

  // Register processing callbacks
  //
  // Create a property map
  AAX_IPropertyMap *const properties = outDesc->NewPropertyMap();
  if (!properties) err = AAX_ERROR_NULL_OBJECT;
  //
  // Generic properties

  uint32_t manu_id = AAXIDfromString(clapDescriptor->vendor);
  uint32_t prod_id = AAXIDfromString(clapDescriptor->id);
  if (aax_plugin_info)
  {
    // optionally override generated manufacturer id
    auto o_manu_id = aax_plugin_info->id_manufacturer;
    if (o_manu_id != 0)
    {
      manu_id = o_manu_id;
    }

    // optionally override generated product id
    auto o_prod_id = aax_plugin_info->id_product;
    if (o_prod_id != 0)
    {
      prod_id = o_prod_id;
    }
  }
  err = properties->AddProperty(AAX_eProperty_ManufacturerID, manu_id);
  err = properties->AddProperty(AAX_eProperty_ProductID, prod_id);
  err = properties->AddProperty(AAX_eProperty_CanBypass, true);
  // err = properties->AddProperty(AAX_eProperty_UsesClientGUI, true);  // true means that it uses auto-GUI by the host, CLAPs have their own UI

  err = properties->AddProperty(AAX_eProperty_RequiresChunkCallsOnMainThread,
                                true);  // for the CLAP this is mandatory
  err = properties->AddProperty(AAX_eProperty_Constraint_Topology,
                                AAX_eConstraintTopology_Monolithic);  // no separate UI and DSP

  // Stem format -specific properties
  err = properties->AddProperty(AAX_eProperty_InputStemFormat, stemformat.format_in);
  err = properties->AddProperty(AAX_eProperty_OutputStemFormat, stemformat.format_out);

  // multi/mono should not be the same
  err = properties->AddProperty(AAX_eProperty_Constraint_MultiMonoSupport, 0);

  // ID properties
  // Use explicit plugin ID from extension if provided, otherwise auto-generate from id + stem name
  uint32_t pluginID = stemformat.plugin_id;
  if (pluginID == 0)
  {
    std::string p(fmt::format("{} - {}", clapDescriptor->id, stemformat.name));
    pluginID = AAXIDfromString(p.c_str());
  }
  err = properties->AddProperty(AAX_eProperty_PlugInID_Native, pluginID);
  err =
      properties->AddProperty(AAX_eProperty_Constraint_Location, AAX_eConstraintLocationMask_DataModel);

  // Register Native callback
  err = outDesc->AddProcessProc_Native<SAAX_Wrapper_AlgorithmicContext>(
      AAXWrapper_AlgorithmProcessProc, properties, AAXWrapper_inInstanceInitProc,
      AAXWrapper_BackgroundProc);
}

/*

  Some explanations on the AAX wording:

  Package:      the plugin dll/bundle
  Effect:       a certain plugin within the dll/bundle. there can more 1+ components in one package
  Component:    the plugins with a certain configuration, like stem formats or different bus settings etc.  

  this is a variant of the DescribeAlgorithmComponent function of the AAX example plugins
  It usually fills out ONE effect, but this is matching only one CHannel/Stemformat configuration

  CLAP can have multiple configurations therefore this function checks for all possible bus/stem configurations
  in a AAX system and creates descriptors for each variant.

  The stem format will be set in the descriptor. EffectInit() is getting the stem format back from the controller
  and initializes the bus configuration. The IDs must differ for each configuration, this will be taken care of here
  by adding a description string to the plugin id and generating appropriate IDs for each Plugin/Stemformat tupel.

  Instead of returning one outDescriptor a lambda function will be called for each descriptor.

*/

static AAX_Result DescribeEffectFromClap(AAX_IEffectDescriptor *outDescriptor,
                                         const Clap::Library *clapFactory, uint32_t plugindex,
                                         const CLAPAAX::plugin_bus_info_t &businfo)
{
  using namespace CLAPAAX;

  const clap_plugin_descriptor_t *clapDescriptor = clapFactory->plugins[plugindex];
  const clap_plugin_info_as_aax_t *aax_plugin_info = nullptr;
  if (clapFactory->_pluginFactoryAAXInfo)
  {
    aax_plugin_info = clapFactory->get_aax_info(plugindex);
  }

  // TODO: list all stem formats

  //MessageBoxA(NULL, "Debugger", "Halted!", MB_OK);
  //_CrtDbgBreak();

  AAX_CheckedResult err;
  AAX_IComponentDescriptor *const compDesc = outDescriptor->NewComponentDescriptor();
  if (!compDesc) err = AAX_ERROR_NULL_OBJECT;

  // add the plugin name(s)
  LOGDETAIL("generating names:");
  auto list = generateShortStrings(clapDescriptor->name);
  for (const auto &e : list)
  {
    LOGDETAIL(e.c_str());
    err = outDescriptor->AddName(e.c_str());
  }

  // get AAX Plugin category uint32_t bitfield from override
  if (aax_plugin_info && aax_plugin_info->aax_features != 0)
  {
    outDescriptor->AddCategory(aax_plugin_info->aax_features);
  }
  else
  {  // , or derive from feature string
    err = outDescriptor->AddCategory(clapCategoriesToAAX(clapDescriptor->features));
  }

  // Effect components

  // Algorithm component
  for (const auto &c : businfo.stemformats)
  {
    // repeat for each bus config

    err = compDesc->Clear();
    DescribeAlgorithmComponent(compDesc, clapFactory, plugindex, aax_plugin_info, businfo, c);
    err = outDescriptor->AddComponent(compDesc);
  }
  // plugin
  err = outDescriptor->AddProcPtr(reinterpret_cast<void *>(ClapAsAAX_Create),
                                  kAAX_ProcPtrID_Create_EffectParameters);

  // GUI
  err = outDescriptor->AddProcPtr((void *)Wrapped_AAX_GUI_Create, kAAX_ProcPtrID_Create_EffectGUI);

#if 0
	// Data model
	err = outDescriptor->AddResourceInfo ( AAX_eResourceType_PageTable, "DemoGainPages.xml" );
	
	// Effect's meter display properties
	//
	// Input meter
	{
		AAX_IPropertyMap* const meterProperties = outDescriptor->NewPropertyMap();
		if ( !meterProperties )
			err = AAX_ERROR_NULL_OBJECT;
		
		err = meterProperties->AddProperty ( AAX_eProperty_Meter_Type, AAX_eMeterType_Input );
		err = meterProperties->AddProperty ( AAX_eProperty_Meter_Orientation, AAX_eMeterOrientation_Default );
		err = outDescriptor->AddMeterDescription( cDemoGain_MeterID[eMeterTap_PreGain], "Input", meterProperties );
	}
	// Output meter
	{
		AAX_IPropertyMap* const meterProperties = outDescriptor->NewPropertyMap();
		if ( !meterProperties )
			err = AAX_ERROR_NULL_OBJECT;
		
		err = meterProperties->AddProperty ( AAX_eProperty_Meter_Type, AAX_eMeterType_Output );
		err = meterProperties->AddProperty ( AAX_eProperty_Meter_Orientation, AAX_eMeterOrientation_Default );
		err = outDescriptor->AddMeterDescription( cDemoGain_MeterID[eMeterTap_PostGain], "Output", meterProperties );
	}
#endif
  return err;
}

AAX_Result GetEffectDescriptions(AAX_ICollection *outCollection)
{
#if 0
  {
    auto pid = GetCurrentProcessId();

    ::MessageBoxA(0, fmt::format("Attach Debugger to Process {}", pid).c_str(), "AAX WRAPPER HALT",
                  MB_OK);
    ::_CrtDbgBreak();
  }
#endif
  AAX_CheckedResult err;

  // get CLAP factory and plugins
  auto *factory = CLAPAAX::guarantee_clap();

  if (factory == nullptr || factory->plugins.empty())
  {
    return AAX_ERROR_NULL_OBJECT;
  }
  // MessageBox(NULL, "ATTACH", "ME", MB_OK);
  // describe the plugins

  if (!factory->plugins.empty())
  {
    // setting up package format
    if (factory->_pluginFactoryAAXInfo)
    {
      outCollection->SetManufacturerName(factory->_pluginFactoryAAXInfo->package_manufacturer);
      outCollection->AddPackageName(factory->_pluginFactoryAAXInfo->package_name);
      outCollection->SetPackageVersion(factory->_pluginFactoryAAXInfo->package_version);
    }
    else
    {
      // use the first plugin name as package name
      auto &plug = factory->plugins[0];
      outCollection->SetManufacturerName(plug->vendor);
      outCollection->AddPackageName(plug->name);
      outCollection->SetPackageVersion(1);
    }

    const uint32_t N = (uint32_t)factory->plugins.size();
    for (uint32_t i = 0; i < N; ++i)
    {
      auto businfo = CLAPAAX::getAvailableBusConfigs(factory, i);

      if (businfo.stemformats.empty())
      {
        LOGINFO("no valid stem formats determined, skipping plugin {}", factory->plugins[i]->id);
        continue;
      }

      AAX_IEffectDescriptor *const effectDescriptor = outCollection->NewDescriptor();

      if (effectDescriptor)
      {
        AAX_SWALLOW_MULT(err = DescribeEffectFromClap(effectDescriptor, factory, i, businfo);

                         // using the clap-plugin id to get it back from the host controller
                         err = outCollection->AddEffect(factory->plugins[i]->id, effectDescriptor););
      }
    }
  }
  else
  {
    err = AAX_ERROR_NULL_OBJECT;
  }

  return err;
}

AAX_CEffectParameters *ClapAsAAX_Create_WithConfig(const char *effect_id, int busconfig)
{
  LOGINFO(
      fmt::format("---- creating AAX wrapper from extension: {} with config {}", effect_id, busconfig));
  auto result = new ClapAsAAX(effect_id, busconfig);
  return result;
}

AAX_CEffectParameters *AAX_CALLBACK ClapAsAAX_Create()
{
  // returning an empty shell
  LOGINFO("-------------------------------------------------------------------------------------");
  return new ClapAsAAX();
}
ClapAsAAX::ClapAsAAX()
  : AAX_CEffectParameters()
  , Clap::IHost()
  , Clap::IAutomation()
  , os::IPlugObject()
  , _os_attached([this] { os::attach(this); }, [this] { os::detach(this); })
{
  ClapAsAAXRegistry::Register(this);
  _activated = false;
}

ClapAsAAX::ClapAsAAX(const char *effectid, int busconfig)
  : AAX_CEffectParameters()
  , Clap::IHost()
  , Clap::IAutomation()
  , os::IPlugObject()
  , _os_attached([this] { os::attach(this); }, [this] { os::detach(this); })
  , _predetermined_effectid(effectid)
  , _predetermined_busconfig(busconfig)
{
  ClapAsAAXRegistry::Register(this);
  _activated = false;
}

ClapAsAAX::~ClapAsAAX()
{
  // Pro Tools does not shut down properly when closed via [X], so we must clean up
  // defensively. Guard on _plugin: if EffectInit never completed the shared_ptr is
  // null and calling into stop/deactivate would dereference it.
  if (_plugin)
  {
    this->stopProcessing();
    this->deactivatePlugin();
  }
  ClapAsAAXRegistry::Unregister(this);
}

static void build_config_request(clap_audio_port_configuration_request *req, uint32_t numchannels,
                                 uint32_t index, bool is_input)
{
  req->is_input = is_input;
  req->port_index = index;
  req->channel_count = numchannels;
  req->port_details = nullptr;  // will be updated if necessary
  switch (numchannels)
  {
    case 1:
      req->port_type = CLAP_PORT_MONO;
      break;
    case 2:
      req->port_type = CLAP_PORT_STEREO;
      break;
    default:
      req->port_type = "unknown";
      break;
  }
}

AAX_Result ClapAsAAX::EffectInit()
{
  using namespace Clap;

  // when this is being called, the plugin is not connected at all, so
  // the actual (AAX) plugin id is being retrieved from the controller.

  //
  AAX_CString m;

  _aax_ctrl = Controller();
  _aax_ctrl->GetEffectID(&m);

  if (!_predetermined_effectid.empty())
  {
    m = _predetermined_effectid;
  }

  LOGINFO(fmt::format("AAX Effect Init for '{}'", m.StdString().c_str()));

  _library = CLAPAAX::guarantee_clap();
  _plugin = Clap::Plugin::createInstance(_library->_pluginFactory, m.StdString(), this);

  if (_plugin)
  {
    if (_plugin->initialize())
    {
      // TODO: initialize calls wrapper specifics and sets up all busses etc.
      {
        if (_plugin->_ext._configurable_audio_ports)
        {
          // configurable - yeah! apply the configuration
          AAX_EStemFormat stem_in, stem_out;
          _aax_ctrl->GetInputStemFormat(&stem_in);
          _aax_ctrl->GetOutputStemFormat(&stem_out);

          auto numInChannels = AAX_STEM_FORMAT_CHANNEL_COUNT(stem_in);
          auto numOutChannels = AAX_STEM_FORMAT_CHANNEL_COUNT(stem_out);
          auto audioports = _plugin->_ext._audioports;

          auto numInPorts = audioports->count(_plugin->_plugin, true);
          auto numOutPorts = audioports->count(_plugin->_plugin, false);

          // building configuration requests for all ports, but configure it with the STEM format
          for (uint32_t i = 0; i < numInPorts; ++i)
          {
            // get the port info
            //clap_audio_port_info_t p;
            //audioports->get(_plugin->_plugin, i, true, &p); <- since port_index is not port.id, the port info is not needed
            clap_audio_port_configuration_request rq;
            build_config_request(&rq, numInChannels, i, true);
            _configuration_requests.emplace_back(rq);
          }

          for (uint32_t i = 0; i < numOutPorts; ++i)
          {
            // get the port info
            clap_audio_port_info_t p;
            audioports->get(_plugin->_plugin, i, false, &p);
            clap_audio_port_configuration_request rq;
            build_config_request(&rq, numOutChannels, i, false);
            _configuration_requests.emplace_back(rq);
          }

          if (!_plugin->_ext._configurable_audio_ports->apply_configuration(
                  _plugin->_plugin, _configuration_requests.data(),
                  (uint32_t)_configuration_requests.size()))
          {
            LOGINFO(fmt::format(
                "audio port configuration could not be applied. Ports {}/{} with {}/{} channels",
                numInPorts, numOutPorts, numInChannels, numOutChannels));
            return AAX_ERROR_NOT_INITIALIZED;
          }
        }
        else
        {
          // when no configurable audio ports exist, the CLAP just works with the
          // given audio port config and predefined audio configuration
        }
      }

      // set samplerate

      // set samplerate
      AAX_CSampleRate sr;
      _aax_ctrl->GetSampleRate(&sr);
      _plugin->setSampleRate(sr);

      // set the blocksizes
      // AAX does not communicate about block sizes at all, which is a weird design decision
      // but since AVID hosts are the only relevant AAX hosts and they use maximum of 1024 samples
      // this is what we provide.
      _plugin->setBlockSizes(gAAXMinBlockSizeInSamples, gAAXMaxBlockSizeInSamples);
      // set signallatency
      _aax_ctrl->SetSignalLatency(0);
    }
  }
  AAX_ASSERT(_activated == false);
  return AAX_SUCCESS;
}

// this is called for each registered field, this is being used to reset the pointer
// to the actual wrapper plugin instance.
AAX_Result ClapAsAAX::ResetFieldData(AAX_CFieldIndex iFieldIndex, void *oData, uint32_t iDataSize) const
{
  //If this is the MonolithicParameters field, let's initialize it to our this pointer.
  if (iFieldIndex == AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mPrivateData))
  {
    LOGINFO("Resetting the private field data pointing back to the wrapper");

    //Make sure everything is at least initialized to 0.
    AAX_ASSERT(iDataSize == sizeof(SAAX_Wrapper_PrivateData));
    memset(oData, 0, iDataSize);

    //Set all of the private data variables.
    SAAX_Wrapper_PrivateData *privatedata = static_cast<SAAX_Wrapper_PrivateData *>(oData);
    privatedata->wrapper = (ClapAsAAX *)this;  // wrap away the weird const of the function declaration
    return AAX_SUCCESS;
  }

  //Call into the base class to clear all other private data.
  return AAX_CEffectParameters::ResetFieldData(iFieldIndex, oData, iDataSize);
}

AAX_Result ClapAsAAX::TimerWakeup()
{
  // Fire any CLAP timers whose period has elapsed.
  if (_plugin && _plugin->_ext._timer)
  {
    auto now = os::getTickInMS();
    for (auto &to : _timerObjects)
    {
      if (to.period_ms > 0 && to.nexttick <= now)
      {
        to.nexttick = now + to.period_ms;
        _plugin->_ext._timer->on_timer(_plugin->_plugin, to.timer_id);
      }
    }
  }
  return AAX_CEffectParameters::TimerWakeup();
}

AAX_Result ClapAsAAX::GetParameterIsAutomatable(AAX_CParamID iParameterID, AAX_CBoolean *itIs) const
{
  auto n = this->_parameterMap.find(iParameterID);
  if (n != _parameterMap.end())
  {
    auto &info = n->second->_clap_param_info;
    *itIs = (info.flags & CLAP_PARAM_IS_AUTOMATABLE);
    return AAX_SUCCESS;
  }
  return AAX_ERROR_INVALID_PARAMETER_ID;
}

AAX_Result ClapAsAAX::GetParameterNumberOfSteps(AAX_CParamID iParameterID, int32_t *aNumSteps) const
{
  auto n = this->_parameterMap.find(iParameterID);
  if (n != _parameterMap.end())
  {
    auto &info = n->second->_clap_param_info;
    if (info.flags & CLAP_PARAM_IS_STEPPED)
    {
      // the number of steps if min=0 and max=1 is 2
      *aNumSteps = 1 + (info.max_value - info.min_value);
    }
    else
    {
      *aNumSteps = 0;  // 0 means discrete/continuous, see class AAX_CParameter
    }

    return AAX_SUCCESS;
  }
  return AAX_ERROR_INVALID_PARAMETER_ID;
}

AAX_Result ClapAsAAX::GetParameterValueString(AAX_CParamID iParameterID, AAX_IString *oValueString,
                                              int32_t iMaxLength) const
{
  return AAX_CEffectParameters::GetParameterValueString(iParameterID, oValueString, iMaxLength);
}

AAX_Result ClapAsAAX::GetParameterValueFromString(AAX_CParamID iParameterID, double *oValuePtr,
                                                  const AAX_IString &iValueString) const
{
  auto n = this->_parameterMap.find(iParameterID);
  if (n != _parameterMap.end())
  {
    if (n->second->_ext_params->text_to_value(_plugin->_plugin, n->second->_clap_param_info.id,
                                              iValueString.Get(), oValuePtr))
    {
      return AAX_SUCCESS;
    }
    else
    {
      return AAX_ERROR_INVALID_STRING_CONVERSION;
    }
  }
  else
  {
    return AAX_ERROR_INVALID_PARAMETER_ID;
  }
}

AAX_Result ClapAsAAX::GetParameterStringFromValue(AAX_CParamID iParameterID, double value,
                                                  AAX_IString *valueString, int32_t maxLength) const
{
  auto n = this->_parameterMap.find(iParameterID);
  if (n != _parameterMap.end())
  {
    char flomf[256];
    if (this->_plugin->_ext._params->value_to_text(_plugin->_plugin, n->second->_clap_param_info.id,
                                                   n->second->asClapValue(value), flomf, sizeof(flomf)))
    {
      *valueString = flomf;
      return AAX_SUCCESS;
    }
    else
      return AAX_ERROR_INVALID_STRING_CONVERSION;
  }
  return AAX_ERROR_INVALID_PARAMETER_ID;
}

AAX_Result ClapAsAAX::GetParameterName(AAX_CParamID iParameterID, AAX_IString *oName) const
{
  auto n = this->_parameterMap.find(iParameterID);
  if (n != _parameterMap.end())
  {
    *oName = n->second->_names.front();
    return AAX_SUCCESS;
  }
  *oName = "wrongid?";
  return AAX_ERROR_UNKNOWN_ID;
}

AAX_Result ClapAsAAX::GetParameterNameOfLength(AAX_CParamID iParameterID, AAX_IString *oName,
                                               int32_t iNameLength) const
{
  AAX_Result aResult = AAX_ERROR_INVALID_STRING_CONVERSION;
  const uint32_t namelen = (uint32_t)iNameLength;

  auto n = this->_parameterMap.find(iParameterID);
  if (n != _parameterMap.end())
  {
    auto &names = n->second->_names;
    const AAX_CString *result = &names.back();
    for (auto i = names.rbegin(); i != names.rend(); ++i)
    {
      if (i->Length() > namelen)
      {
        oName->Set(result->StdString().c_str());
        return AAX_SUCCESS;
      }
      result = &(*i);
    }
    oName->Set(result->StdString().c_str());
    return AAX_SUCCESS;
  }
  return aResult;
}

AAX_Result ClapAsAAX::UpdateParameterNormalizedValue(AAX_CParamID iParameterID, double iValue,
                                                     AAX_EUpdateSource iSource)
{
  // this needs to be overridden. The default implementation just stores the value
  // locally, but we need to pass this into the stream

  // and yeah, no timestamps for this, so we get the parameter and pass its ID, cookie and the new value

  auto p = _parameterMap.find(iParameterID);
  if (p == _parameterMap.end()) return AAX_ERROR_INVALID_PARAMETER_ID;
  auto *ptr = p->second.get();

  _paramsToProcess.push(
      {ptr->_clap_param_info.id, ptr->asClapValue(iValue), ptr->_clap_param_info.cookie});

  // calling the base class makes sure that things like numParameterChanges are being updated
  return AAX_CEffectParameters::UpdateParameterNormalizedValue(iParameterID, iValue, iSource);
}

static const AAX_CTypeID CLAP_STATE_CHUNK_ID = 'clap';

AAX_Result ClapAsAAX::GetNumberOfChunks(int32_t *oNumChunks) const
{
  // TODO: Return 1 (and only 1) chunk
  // return AAX_CEffectParameters::GetNumberOfChunks(oNumChunks);
  *oNumChunks = 1;
  return AAX_SUCCESS;
}

AAX_Result ClapAsAAX::GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID *oChunkID) const
{
  if (iIndex != 0)
  {
    *oChunkID = AAX_CTypeID(0);
    return AAX_ERROR_INVALID_CHUNK_INDEX;
  }

  *oChunkID = CLAP_STATE_CHUNK_ID;
  return AAX_SUCCESS;
}

AAX_Result ClapAsAAX::GetChunkSize(AAX_CTypeID iChunkID, uint32_t *oSize) const
{
  if (iChunkID != CLAP_STATE_CHUNK_ID) return AAX_ERROR_INVALID_CHUNK_ID;

  // This method is invoked every time a chunk is saved, therefore it is possible to have dynamically sized chunks.
  // However, note that each call to GetChunkSize() will correspond to a following call to GetChunk().
  // The chunk provided in GetChunk() must have the same size as the size provided by GetChunkSize().

  _state.clear();
  if (_plugin->_ext._state->save(_plugin->_plugin, _state))
  {
    *oSize = static_cast<uint32_t>(_state.size());
    return AAX_SUCCESS;
  }

  return AAX_ERROR_INCORRECT_CHUNK_SIZE;
}

AAX_Result ClapAsAAX::GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk *oChunk) const
{
  // Fills a block of data with chunk information representing the plug-in's current state.

  // By calling this method, the host is requesting information about the current state of the plug-in. The following chunk fields should be explicitly populated in this method. Other fields will be populated by the host.
  //
  // AAX_SPlugInChunk::fData
  // AAX_SPlugInChunk::fVersion
  // AAX_SPlugInChunk::fName (Optional)
  // AAX_SPlugInChunk::fSize (Data size only)

  if (iChunkID != CLAP_STATE_CHUNK_ID) return AAX_ERROR_INVALID_CHUNK_ID;

  oChunk->fVersion = 1;
  memset(oChunk->fName, 0, 32);  //Just in case, lets make sure unused chars are null.
  memcpy(oChunk->fName, "clap-as-aax-state", 11);
  oChunk->fSize = (int32_t)_state.size();
  memcpy(oChunk->fData, _state.data(), _state.size());

  return AAX_SUCCESS;
}

AAX_Result ClapAsAAX::SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk *iChunk)
{
  if (iChunkID != CLAP_STATE_CHUNK_ID) return AAX_ERROR_INVALID_CHUNK_ID;

  _paramsToProcess.clear();

  auto data = (const uint8_t *)(iChunk->fData);
  _state.setData(data, iChunk->fSize);
  if (_plugin->_ext._state->load(_plugin->_plugin, _state))
  {
    return AAX_SUCCESS;
  }
  return AAX_ERROR_MALFORMED_CHUNK;
}

AAX_Result ClapAsAAX::NotificationReceived(AAX_CTypeID inNotificationType,
                                           const void *inNotificationData,
                                           uint32_t inNotificationDataSize)
{
  // TODO: check for several notifications from the host
  switch (inNotificationType)
  {
    case AAX_eNotificationEvent_SideChainBeingConnected:

      break;
    case AAX_eNotificationEvent_SideChainBeingDisconnected:
      break;
    case AAX_eNotificationEvent_SignalLatencyChanged:
    {
      int32_t newLatency;
      if (_aax_ctrl->GetSignalLatency(&newLatency) == AAX_SUCCESS)
      {
        // from now on, the new latency is active
      }
    }
    break;
    case AAX_eNotificationEvent_TrackNameChanged:
      break;
    case AAX_eNotificationEvent_PresetOpened:
      break;
    case AAX_eNotificationEvent_SessionBeingOpened:
      break;
    case AAX_eNotificationEvent_EnteringOfflineMode:
      break;
    case AAX_eNotificationEvent_ExitingOfflineMode:
      break;
    case AAX_eNotificationEvent_SessionPathChanged:
      break;
    case AAX_eNotificationEvent_MaxViewSizeChanged:
      break;
    default:
      break;
  }
  // but pass on
  return AAX_CEffectParameters::NotificationReceived(inNotificationType, inNotificationData,
                                                     inNotificationDataSize);
}

void ClapAsAAX::setupWrapperSpecifics(const clap_plugin_t *plugin)
{
  // nothing for AAX yet
}

void ClapAsAAX::setupAudioBusses(const clap_plugin_t *plugin,
                                 const clap_plugin_audio_ports_t *audioports)
{
  // the busses are already declared by the stem configuration of the instance
  // any further setup does happen in the AAXProcessAdapter
}

void ClapAsAAX::setupMIDIBusses(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *noteports)
{
  if (noteports->count(plugin, true) > 0)
  {
    clap_note_port_info_t info;
    if (noteports->get(plugin, 0, true, &info))
    {
      this->_midi_first_portid = info.id;
      this->_midi_prefer_mididialect = (info.preferred_dialect & CLAP_NOTE_DIALECT_MIDI);
    }
  }
}

void ClapAsAAX::setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params)
{
  if (!params) return;

  auto numparams = params->count(plugin);
  _paramsToProcess.init(numparams * 4);

  for (decltype(numparams) i = 0; i < numparams; ++i)
  {
    clap_param_info info;
    if (params->get_info(plugin, i, &info))
    {
      if (info.flags & CLAP_PARAM_IS_HIDDEN) continue;

      std::string paramname;

      if (info.module[0])
      {
        // ignore leading '/'
        if (info.module[0] == '/')
          paramname = info.module + 1;
        else
          paramname = info.module;

        paramname.push_back('/');
      }
      paramname.append(info.name);

      auto id = createAAXId(info.id);

      auto wrappedParam = std::make_shared<AAXWrappedParameterInfo_t>(this->_plugin->_plugin, info, id);

      auto n = generateShortStrings(paramname);
      wrappedParam->_names.reserve(n.size());
      for (const auto &i : n)
      {
        wrappedParam->_names.emplace_back(AAX_CString(i));
      }

      // now to the lookup maps
      _parameterMap[id] = wrappedParam;
      _parameterMapCLAP[info.id] = wrappedParam;

      auto p = new AAX_CParameter<double>(
          _parameterMap[id]->_aax_identifier.c_str(), AAX_CString(paramname),
          wrappedParam->asAAXValue(info.default_value), AAX_CLinearTaperDelegate<double>(0, 1),
          AAX_ClapParamDisplayDelegate(wrappedParam), info.flags & CLAP_PARAM_IS_AUTOMATABLE);
      mParameterManager.AddParameter(p);

      // get the index and store it for fast retrieval
      wrappedParam->_paramAAXIndex = mParameterManager.GetParameterIndex(id.c_str());
    }
  }
  AAX_ASSERT(_activated == false);
}

void ClapAsAAX::param_rescan(clap_param_rescan_flags flags)
{
  // AAX does not support adding/removing parameters at runtime, so only the TEXT
  // flag (display name changes) can be honoured — via AAX_CParameter::SetName(),
  // which calls mAutomationDelegate->ParameterNameChanged() internally and triggers
  // Pro Tools to refresh the name everywhere it is displayed.
  if (!(flags & CLAP_PARAM_RESCAN_TEXT)) return;

  if (!_plugin || !_plugin->_ext._params) return;

  uint32_t count = _plugin->_ext._params->count(_plugin->_plugin);
  for (uint32_t i = 0; i < count; ++i)
  {
    clap_param_info_t info;
    if (!_plugin->_ext._params->get_info(_plugin->_plugin, i, &info)) continue;

    auto it = _parameterMapCLAP.find(info.id);
    if (it == _parameterMapCLAP.end()) continue;

    auto &wrapped = *it->second;

    // Update our cached copy so display delegates stay consistent.
    strncpy(wrapped._clap_param_info.name, info.name, CLAP_NAME_SIZE - 1);
    wrapped._clap_param_info.name[CLAP_NAME_SIZE - 1] = '\0';

    // Notify AAX; SetName() calls mAutomationDelegate->ParameterNameChanged() internally.
    AAX_IParameter *aaxParam = mParameterManager.GetParameterByID(wrapped._aax_identifier.c_str());
    if (aaxParam) aaxParam->SetName(AAX_CString(info.name));
  }
}

void ClapAsAAX::param_clear(clap_id /*param*/, clap_param_clear_flags /*flags*/)
{
  // AAX provides no mechanism to clear automation or modulation data for a
  // specific parameter programmatically. Nothing to do.
}

void ClapAsAAX::param_request_flush()
{
  // Signal onIdle() to call the params flush extension on the next main-thread tick.
  _flushRequested.store(true);
}

bool ClapAsAAX::gui_can_resize()
{
  if (!_plugin) return false;

  auto g = _plugin->_ext._gui;
  if (!g) return false;

  auto res = g->can_resize(_plugin->_plugin);
  return res;
}

bool ClapAsAAX::gui_request_resize(uint32_t width, uint32_t height)
{
  if (this->_aax_view)
  {
    if (this->_aax_view->setWindowSize(width, height))
    {
      return true;
    }
  }
  return false;
}

bool ClapAsAAX::gui_request_show()
{
  return false;
}

bool ClapAsAAX::gui_request_hide()
{
  return false;
}

void ClapAsAAX::latency_changed()
{
  _aax_ctrl->SetSignalLatency(_plugin->_ext._latency->get(_plugin->_plugin));
  // will be signalled from the host with a latency notification
  // see AAX_eNotificationEvent_SignalLatencyChanged
}

void ClapAsAAX::tail_changed()
{
  // no equivalent tO AAX
}

void ClapAsAAX::mark_dirty()
{
  // just pretending there is a change is enough for setting dirty
  ++mNumPlugInChanges;
}

void ClapAsAAX::restartPlugin()
{
}

bool ClapAsAAX::register_timer(uint32_t period_ms, clap_id *timer_id)
{
  // AAX TimerWakeup fires at roughly 30ms; clamp period to that minimum.
  if (period_ms < 30) period_ms = 30;

  auto now = os::getTickInMS();

  // Reuse an existing slot if one is free.
  for (size_t i = 0; i < _timerObjects.size(); ++i)
  {
    auto &to = _timerObjects[i];
    if (to.period_ms == 0)
    {
      to.timer_id = static_cast<clap_id>(i + 1000);
      to.period_ms = period_ms;
      to.nexttick = now + period_ms;
      *timer_id = to.timer_id;
      return true;
    }
  }

  // No free slot — create a new one.
  auto newid = static_cast<clap_id>(_timerObjects.size() + 1000);
  _timerObjects.push_back({period_ms, now + period_ms, newid});
  *timer_id = newid;
  return true;
}

bool ClapAsAAX::unregister_timer(clap_id timer_id)
{
  for (auto &to : _timerObjects)
  {
    if (to.timer_id == timer_id)
    {
      to.period_ms = 0;
      to.nexttick = 0;
      return true;
    }
  }
  return false;
}

bool ClapAsAAX::track_info_get(clap_track_info_t *info)
{
  return false;
}

const char *ClapAsAAX::host_get_name()
{
  AAX_IController *ctrl = Controller();
  AAX_CString hostname;
  if (AAX_SUCCESS == ctrl->GetHostName(&hostname))
  {
    _wrapper_hostname = hostname.StdString();
    _wrapper_hostname.append(" (CLAP-as-AAX)");
  }
  return _wrapper_hostname.c_str();
}

bool ClapAsAAX::supportsContextMenu() const
{
  return false;
}

bool ClapAsAAX::context_menu_populate(const clap_context_menu_target_t *target,
                                      const clap_context_menu_builder_t *builder)
{
  return false;
}

bool ClapAsAAX::context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id)
{
  return false;
}

bool ClapAsAAX::context_menu_can_popup()
{
  return false;
}

bool ClapAsAAX::context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index,
                                   int32_t x, int32_t y)
{
  return false;
}

void ClapAsAAX::request_callback()
{
  _wants_on_main_thread.store(true);
}

void ClapAsAAX::onIdle()
{
  if (_flushRequested.exchange(false))
  {
    auto fo = _plugin->AlwaysMainThread();
    if (_processAdapter) _processAdapter->flush();
  }

  // process requests etc. on mainthread etc.
  if (_wants_on_main_thread.exchange(false))
  {
    // this IS the main thread
    auto fo = _plugin->AlwaysMainThread();
    _plugin->_plugin->on_main_thread(_plugin->_plugin);
  }
}

void ClapAsAAX::activatePlugin()
{
  if (!_activated)
  {
    _gesturedparameters.reserve(8192);

    _processAdapter = std::make_unique<AAXProcessAdapter>();
    _processAdapter->setupProcessing(_plugin->_plugin, _plugin->getSampleRate(), _plugin->_ext._params,
                                     _plugin->_ext._audioports, this, _gesturedparameters,
                                     _paramsToProcess, _midi_first_portid, _midi_prefer_mididialect);

    _activated = true;
    _plugin->activate();

    // pass latency when activated
    auto scope = _plugin->AlwaysMainThread();
    auto newlatency = _plugin->_ext._latency->get(_plugin->_plugin);
    if (newlatency != _latency)
    {
      _latency = newlatency;
      _aax_ctrl->SetSignalLatency(_latency);
    }
  }
}

void ClapAsAAX::deactivatePlugin()
{
  if (_activated)
  {
    _activated = false;
    _plugin->deactivate();
    _processAdapter.reset();
  }
}

void ClapAsAAX::startProcessing()
{
  if (!_processing)
  {
    _processing = true;
    _plugin->start_processing();
  }
}

void ClapAsAAX::stopProcessing()
{
  if (_processing)
  {
    _processing = false;
    _plugin->stop_processing();
  }
}

void ClapAsAAX::onBeginEdit(clap_id id)
{
  auto p = _parameterMapCLAP.find(id);
  if (p != _parameterMapCLAP.end())
  {
    mParameterManager.GetParameter(p->second->_paramAAXIndex)->Touch();
  }
}

void ClapAsAAX::onPerformEdit(const clap_event_param_value_t *value)
{
  auto p = _parameterMapCLAP.find(value->param_id);
  if (p != _parameterMapCLAP.end())
  {
    auto *param = p->second.get();
    mParameterManager.GetParameter(param->_paramAAXIndex)
        ->SetNormalizedValue(param->asAAXValue(value->value));
  }
}

void ClapAsAAX::onEndEdit(clap_id id)
{
  auto p = _parameterMapCLAP.find(id);
  if (p != _parameterMapCLAP.end())
  {
    mParameterManager.GetParameter(p->second->_paramAAXIndex)->Release();
  }
}
