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
    for (auto *inst : GetSet())
    {
      if (inst == instance)
      {
        return true;
      }
    }
    return false;
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
      os::log("adding new instance");
      self->activatePlugin();
      self->startProcessing();
      break;
    case AAX_eComponentInstanceInitAction_RemovingInstance:
      os::log("removing instance");
      if (ClapAsAAXRegistry::Exists(self))
      {
        self->stopProcessing();
        self->deactivatePlugin();
      }
      else
      {
        os::log("instance to be removed does not exists anymore");
      }
      break;
    case AAX_eComponentInstanceInitAction_ResetInstance:
      os::log("resetting instance!?");
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

// AAX needs all the description for a component in advance - there is no dynamic thing in here.
static void DescribeAlgorithmComponent(AAX_IComponentDescriptor *outDesc,
                                       const clap_plugin_descriptor_t *clapDescriptor)
{
  AAX_CheckedResult err;

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

  if (false)  // setupInfo.mNeedsGlobalMIDI)
  {
    err = outDesc->AddMIDINode(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mInputNode),
                               AAX_eMIDINodeType_LocalInput, "MIDI IN", 0xF);
    err = outDesc->AddMIDINode(AAX_FIELD_INDEX(SAAX_Wrapper_AlgorithmicContext, mOutputNode),
                               AAX_eMIDINodeType_LocalOutput, "MIDI OUT", 0xF);
    err = outDesc->AddMIDINode(globalNodeID, AAX_eMIDINodeType_Global,
                               "MIDI Global" /*setupInfo.mGlobalMIDINodeName */,
                               1 /*setupInfo.mGlobalMIDIEventMask*/);
  }
  else
    err = outDesc->AddPrivateData(
        globalNodeID, sizeof(float),
        AAX_ePrivateDataOptions_DefaultOptions);  //Just here to fill the port.  Not used.

  if (true)  // setupInfo.mNeedsInputMIDI)
    err = outDesc->AddMIDINode(localInputNodeID, AAX_eMIDINodeType_LocalInput,
                               "MIDI IN" /*setupInfo.mInputMIDINodeName*/,
                               0xF /*setupInfo.mInputMIDIChannelMask*/);
  else
    err = outDesc->AddPrivateData(
        localInputNodeID, sizeof(float),
        AAX_ePrivateDataOptions_DefaultOptions);  //Just here to fill the port.  Not used.

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

#if 0
	err = outDesc->AddAudioIn (eAlgFieldID_AudioIn);
	err = outDesc->AddAudioOut (eAlgFieldID_AudioOut);
	err = outDesc->AddAudioBufferLength (eAlgFieldID_BufferSize);
	static_assert(eMeterTap_Count == sizeof(cDemoGain_MeterID)/sizeof(AAX_CTypeID), "unexpected meter tap array size");
	err = outDesc->AddMeters ( eAlgFieldID_Meters, cDemoGain_MeterID, eMeterTap_Count );
    //
	// Register context fields as communications destinations (i.e. input)
	err = outDesc->AddDataInPort ( eAlgPortID_BypassIn, sizeof (int32_t) );
	err = outDesc->AddDataInPort ( eAlgPortID_CoefsGainIn, sizeof (SDemoGain_CoefsGain) );

#endif

  // Register processing callbacks
  //
  // Create a property map
  AAX_IPropertyMap *const properties = outDesc->NewPropertyMap();
  if (!properties) err = AAX_ERROR_NULL_OBJECT;
  //
  // Generic properties

  err = properties->AddProperty(AAX_eProperty_ManufacturerID, AAXIDfromString(clapDescriptor->vendor));
  err = properties->AddProperty(AAX_eProperty_ProductID, AAXIDfromString(clapDescriptor->id));
  err = properties->AddProperty(AAX_eProperty_CanBypass, true);
  // err = properties->AddProperty(AAX_eProperty_UsesClientGUI, true);  // Uses auto-GUI by the host

  err = properties->AddProperty(AAX_eProperty_RequiresChunkCallsOnMainThread,
                                true);  // for the CLAP this is mandatory
  err = properties->AddProperty(AAX_eProperty_Constraint_Topology,
                                AAX_eConstraintTopology_Monolithic);  // no separate UI and DSP
  //
  //
  // Stem format -specific properties
  err = properties->AddProperty(AAX_eProperty_InputStemFormat, AAX_eStemFormat_Stereo);
  err = properties->AddProperty(AAX_eProperty_OutputStemFormat, AAX_eStemFormat_Stereo);

  // multi/mono should not be the same
  err = properties->AddProperty(AAX_eProperty_Constraint_MultiMonoSupport, 0);
  //
  // ID properties
  std::string p(clapDescriptor->id);
  // TODO: enumerate bus combinations

  p.append("-stereo");

  err = properties->AddProperty(AAX_eProperty_PlugInID_Native,
                                AAXIDfromString(p.c_str()));  // cDemoGain_PlugInID_Native
  err =
      properties->AddProperty(AAX_eProperty_Constraint_Location, AAX_eConstraintLocationMask_DataModel);

  //	err = properties->AddProperty ( AAX_eProperty_PlugInID_AudioSuite, cDemoGain_PlugInID_AudioSuite );	// for offline processing
  // 	err = properties->AddProperty ( AAX_eProperty_PlugInID_TI, cDemoGain_PlugInID_TI );

  // Register Native callback
  err = outDesc->AddProcessProc_Native<SAAX_Wrapper_AlgorithmicContext>(
      AAXWrapper_AlgorithmProcessProc, properties, AAXWrapper_inInstanceInitProc,
      AAXWrapper_BackgroundProc);

#if 0
	no TI in clap
	// TI-specific properties
#ifndef AAX_TI_BINARY_IN_DEVELOPMENT  // Define this macro when using a debug TI DLL to allocate only 1 instance per chip
	err = properties->AddProperty ( AAX_eProperty_TI_InstanceCycleCount, 102 );
	err = properties->AddProperty ( AAX_eProperty_TI_SharedCycleCount, 70 );
#endif
	err = properties->AddProperty ( AAX_eProperty_DSP_AudioBufferLength, AAX_eAudioBufferLengthDSP_Default );
	
	// Register TI callback
	err = outDesc->AddProcessProc_TI ("DemoGain_MM_TI_Example.dll", "AlgEntry", properties );
#endif
}

/*

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
                                         const clap_plugin_descriptor_t *clapDescriptor)
{
  using namespace CLAPAAX;

  // TODO: list all stem formats

  // MessageBoxA(NULL, "Debugger", "Halted!", MB_OK);

  AAX_CheckedResult err;
  AAX_IComponentDescriptor *const compDesc = outDescriptor->NewComponentDescriptor();
  if (!compDesc) err = AAX_ERROR_NULL_OBJECT;

  // add the plugin name(s)
  os::log("generating names:");
  auto list = generateShortStrings(clapDescriptor->name);
  for (const auto &e : list)
  {
    os::log(e.c_str());
    err = outDescriptor->AddName(e.c_str());
  }

  err = outDescriptor->AddCategory(clapCategoriesToAAX(clapDescriptor->features));

  // Effect components
  //
  //
  //
  // Algorithm component
  {
    // repeat for each bus config
    err = compDesc->Clear();
    DescribeAlgorithmComponent(compDesc, clapDescriptor);
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

// TODO: we will change this in the future.
namespace cfg
{
clap_audio_port_configuration_request mono_out[]{{false, 0, 1, CLAP_PORT_MONO, nullptr}};

const clap_audio_port_configuration_request mono_in_out[]{
    {true, 0, 1, CLAP_PORT_MONO, nullptr},
    {false, 0, 1, CLAP_PORT_MONO, nullptr},
};

const clap_audio_port_configuration_request mono_in_stereo_out[]{
    {true, 0, 1, CLAP_PORT_MONO, nullptr},
    {false, 0, 2, CLAP_PORT_STEREO, nullptr},
};

const clap_audio_port_configuration_request mono_inx2_stereo_out[]{
    {true, 0, 2, CLAP_PORT_MONO, nullptr},
    {false, 0, 2, CLAP_PORT_STEREO, nullptr},
};

const clap_audio_port_configuration_request stereo_out[]{
    {false, 0, 2, CLAP_PORT_STEREO, nullptr},
};

const clap_audio_port_configuration_request stereo_in_out[]{
    {true, 0, 2, CLAP_PORT_STEREO, nullptr},
    {false, 0, 2, CLAP_PORT_STEREO, nullptr},
    // {false, 1, 2, CLAP_PORT_STEREO, nullptr},
};

//const clap_audio_port_configuration_request in2_out2[]{
//    {true, 0, 2, "", nullptr},
//    {false, 0, 2, "", nullptr},
//};
//
//const clap_audio_port_configuration_request in2x2_out2[]{
//    {true, 0, 2, CLAP_PORT_STEREO, nullptr},
//    {true, 1, 2, CLAP_PORT_STEREO, nullptr},
//    {false, 0, 2, CLAP_PORT_STEREO, nullptr},
//};

struct request_t
{
  const char *label;
  const clap_audio_port_configuration_request *ptr;
  const int count;
} request[] = {{"mono-out", mono_out, 1},
               {"stereo-out", stereo_out, 1},
               {"mono-in,mono-out", mono_in_out, 2},
               {"stereo-in,stereo-out", stereo_in_out, 2},
               {"mono-in,stereo-out", mono_in_stereo_out, 2},
               {"2mono-in,stereo-out", mono_inx2_stereo_out, 2},
               //{"2in-2out-unspecified", in2_out2, 2},
               // {"2x2in-2out-unspecified", in2x2_out2, 3},

               {nullptr, nullptr, 0}};

#if 0
const uint8_t channelmap_quad4[4]{
  CLAP_SURROUND_FL,
  CLAP_SURROUND_FR,
  CLAP_SURROUND_BR,
  CLAP_SURROUND_BL,
};

const clap_audio_port_configuration_request quad40_i_o[]{
    {true, 0, 4, CLAP_PORT_SURROUND, channelmap_quad4},
    {false, 0, 4, CLAP_PORT_SURROUND, channelmap_quad4},
};

const clap_audio_port_configuration_request mono4[]{
    {true, 0, 1, CLAP_PORT_MONO, nullptr},
    {true, 1, 1, CLAP_PORT_MONO, nullptr},
    {true, 2, 1, CLAP_PORT_MONO, nullptr},
    {true, 3, 1, CLAP_PORT_MONO, nullptr},
    {false, 0, 1, CLAP_PORT_MONO, nullptr},
    {false, 1, 1, CLAP_PORT_MONO, nullptr},
    {false, 2, 1, CLAP_PORT_MONO, nullptr},
    {false, 3, 1, CLAP_PORT_MONO, nullptr},

};
#endif

}  // namespace cfg

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

  using configrequests_t = std::vector<clap_audio_port_configuration_request>;

  if (!factory->plugins.empty())
  {
    for (const auto i : factory->plugins)
    {
      std::vector<configrequests_t> configs;

#if 1
      // why here? because we have factory and the clap-id
      static const clap_host_params_t micro_params = {
          [](const clap_host_t *host, clap_param_rescan_flags flags) -> void {},
          [](const clap_host_t *host, clap_id param_id, clap_param_clear_flags flags) -> void {},
          [](const clap_host_t *host) -> void {}};
      static const clap_host_audio_ports_t micro_audio_ports = {
          [](const clap_host_t *host, uint32_t flag) -> bool { return false; },
          [](const clap_host_t *host, uint32_t flags) -> void {}};
      clap_host_t microhost = {
          CLAP_VERSION,
          nullptr,
          "aax_scanner",
          "clap_wrapper",
          "",
          "1.0",
          [](const struct clap_host *host, const char *extension_id) -> const void *
          {
            if (extension_id == nullptr) return nullptr;
            os::log(extension_id);
            if (!strcmp(CLAP_EXT_PARAMS, extension_id)) return &micro_params;
            if (!strcmp(CLAP_EXT_AUDIO_PORTS, extension_id)) return &micro_audio_ports;
            return nullptr;
          },
          [](const struct clap_host *host) -> void {},  // request_restart
          [](const struct clap_host *host) -> void {},  // request_process
          [](const struct clap_host *host) -> void {},  // request_callback
      };

      try
      {
        // create a temporary plugin instance ------------------
        auto *tmpplug =
            factory->_pluginFactory->create_plugin(factory->_pluginFactory, &microhost, i->id);
        try
        {
          tmpplug->init(tmpplug);
          auto ext_aud =
              (clap_plugin_audio_ports *)(tmpplug->get_extension(tmpplug, CLAP_EXT_AUDIO_PORTS));
          auto ext_cap = (clap_plugin_configurable_audio_ports_t *)(tmpplug->get_extension(
              tmpplug, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS));

          auto ext_sur = (clap_plugin_surround_t *)(tmpplug->get_extension(tmpplug, CLAP_EXT_SURROUND));

          // build a bus setting ------------------
          configrequests_t requests;
          // bool standardconfig_is_mono_or_stereo = true;

          if (ext_aud && ext_cap)
          {
            // collect input definitions for each audio bus
            uint32_t numins = ext_aud->count(tmpplug, true);
            uint32_t numout = ext_aud->count(tmpplug, false);
            for (uint32_t i = 0; i < numins; ++i)
            {
              clap_audio_port_info_t info;
              if (ext_aud->get(tmpplug, i, true, &info))
              {
                // {true, 0, 1, CLAP_PORT_MONO, nullptr},
                requests.emplace_back(clap_audio_port_configuration_request{true, i, info.channel_count,
                                                                            info.port_type, nullptr});
              }
            }
            // collect output definition for each audio bus
            for (uint32_t i = 0; i < numout; ++i)
            {
              clap_audio_port_info_t info;
              if (ext_aud->get(tmpplug, i, true, &info))
              {
                // {false, 0, 1, CLAP_PORT_MONO, nullptr},
                requests.emplace_back(clap_audio_port_configuration_request{false, i, info.channel_count,
                                                                            info.port_type, nullptr});
              }
            }

            bool standardconfig_is_surround = false;

            for (auto &m : requests)
            {
              if (!strcmp(m.port_type, CLAP_PORT_MONO)) continue;
              if (!strcmp(m.port_type, CLAP_PORT_STEREO)) continue;
              // whatever the plugin says, less than 3 channels are MONO or STEREO
              if (m.channel_count <= 2) continue;

              os::log("default configuration is not mono or stereo\n");
              // standardconfig_is_mono_or_stereo = false;
              if (!strcmp(m.port_type, CLAP_PORT_SURROUND))
              {
                standardconfig_is_surround = true;
              }
              break;
            }

            if (ext_sur && standardconfig_is_surround)
            {
              uint8_t mapinfo[64];
              auto chancnt = ext_sur->get_channel_map(tmpplug, 1, 0, mapinfo, 64);
              if (chancnt == requests[0].channel_count)
              {
                os::log("got an channel map of {} entries", chancnt);
              }
            }

            // checking all MONO
            for (auto &m : requests)
            {
              m.channel_count = 1;
              m.port_details = nullptr;
              m.port_type = CLAP_PORT_MONO;
            }

            if (ext_cap->can_apply_configuration(tmpplug, &requests[0], (uint32_t)requests.size()))
            {
              configs.emplace_back(requests);
            }

            // checking all STEREO
            for (auto &m : requests)
            {
              m.channel_count = 2;
              m.port_details = nullptr;
              m.port_type = CLAP_PORT_STEREO;
            }
            if (ext_cap->can_apply_configuration(tmpplug, &requests[0], (uint32_t)requests.size()))
            {
              configs.emplace_back(requests);
            }
            //
            // checking all configs as long there is a name for it
            //while (rq->label)
            //{
            //  if (ext_cap->can_apply_configuration(tmpplug, rq->ptr, rq->count))
            //  {
            //    configs.emplace_back(rq->label);
            //  }
            //  ++rq;
            //}
          }
          os::log(fmt::format("the following configurations have been determined for plugin {}:",
                              tmpplug->desc->name));
          for (auto &i : configs)
          {
            os::log("--------------");
            for (auto &c : i)
            {
              os::log(fmt::format("  #{} {} {} with {} channels", c.port_index,
                                  c.is_input ? "IN" : "OUT", c.port_type, c.channel_count));
            }
          }
        }
        catch (...)
        {
          os::log("something got totally wrong");
        }
        tmpplug->destroy(tmpplug);
      }
      catch (std::exception &e)
      {
        os::log(e.what());
      }
#endif
      AAX_IEffectDescriptor *const effectDescriptor = outCollection->NewDescriptor();

      if (effectDescriptor)
      {
        AAX_SWALLOW_MULT(err = DescribeEffectFromClap(effectDescriptor, i);

                         // using the clap-plugin id to get it back from the host controller
                         err = outCollection->AddEffect(i->id, effectDescriptor););
      }
    }

    // use the first plugin name as package name
    auto &plug = factory->plugins[0];
    outCollection->SetManufacturerName(plug->vendor);
    outCollection->AddPackageName(plug->name);
    outCollection->SetPackageVersion(1);
  }
  else
  {
    err = AAX_ERROR_NULL_OBJECT;
  }

  return err;
}

AAX_CEffectParameters *AAX_CALLBACK ClapAsAAX_Create()
{
  // returning an empty shell
  os::log("-------------------------------------------------------------------------------------");
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

ClapAsAAX::~ClapAsAAX()
{
  // Protools does not shut down properly and when just being closed by click on [X].
  // therefore we need to clean

  this->stopProcessing();
  this->deactivatePlugin();
  ClapAsAAXRegistry::Unregister(this);
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

  os::log(fmt::format("AAX Effect Init for '{}'", m.StdString().c_str()));

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

          auto numInputs = AAX_STEM_FORMAT_CHANNEL_COUNT(stem_in);
          auto numOutputs = AAX_STEM_FORMAT_CHANNEL_COUNT(stem_out);

          // TODO: this will be useful later
          (void)numInputs;
          (void)numOutputs;
        }
      }
      AAX_EStemFormat p;
      _aax_ctrl->GetInputStemFormat(&p);
      // AAX_eStemFormat_Mono
      // AAX_eStemFormat_Stereo
      if (p == AAX_eStemFormat_Mono)
      {
      }
      // TODO: set samplerate and initialize bus format

      AAX_CSampleRate sr;
      _aax_ctrl->GetSampleRate(&sr);
      _plugin->setSampleRate(sr);

      _plugin->setBlockSizes(32, 1024);
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
    os::log("Resetting the private field data pointing back to the wrapper");

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
  // note: this is neither mainthread nor audiothread
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
  memcpy(oChunk->fName, "clap-as-aax", 11);
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
        // this is the external output latency, we can't to this yet
        // TODO: set output latency
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
}

void ClapAsAAX::param_clear(clap_id param, clap_param_clear_flags flags)
{
}

void ClapAsAAX::param_request_flush()
{
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
  return false;
}

bool ClapAsAAX::unregister_timer(clap_id timer_id)
{
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
    _processAdapter = std::make_unique<AAXProcessAdapter>();
    _processAdapter->setupProcessing(_plugin->_plugin, _plugin->getSampleRate(), _plugin->_ext._params,
                                     _plugin->_ext._audioports, this, _paramsToProcess,
                                     _midi_first_portid, _midi_prefer_mididialect);

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
