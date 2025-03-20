#include <clap/clap.h>
#include <clap/ext/gui.h>
#include "wrapasaax.h"  // Header that declares the ClapAsAAX class
#include "AAX_CEffectParameters.h"
#include "AAX_IViewContainer.h"
#include <fstream>

ClapAsAAX::ClapAsAAX() : _library(nullptr), _libraryIndex(0), _plugin(nullptr)
{
  _processAdapter = new Clap::AAX::ProcessAdapter();
}

ClapAsAAX::~ClapAsAAX() = default;

//---AAX--------------------------------------------------------------------------------

AAX_Result ClapAsAAX::GetEffectDescriptions(AAX_ICollection* outCollection)
{
  // Debug log: append a line to /tmp/get_effect_descriptions.log
  std::ofstream logFile("/tmp/get_effect_descriptions.log", std::ios::app);
  if (logFile.is_open())
  {
    logFile << "GetEffectDescriptions called\n";
    logFile.close();
  }

  outCollection->SetManufacturerName("ManufacturerName");
  outCollection->AddPackageName("PackageName");
  outCollection->SetPackageVersion(1);

  return AAX_SUCCESS;
}

//---AAX Data Model--------------------------------------------------------------------------------
AAX_CEffectParameters* AAX_CALLBACK ClapAsAAX::Create()
{
  return new ClapAsAAX();
}

// AAX_CEffectParameters overrides

AAX_Result ClapAsAAX::EffectInit()
{
  if (!_plugin)
  {
    _plugin = Clap::Plugin::createInstance(*_library, _libraryIndex, this);
  }

  // Tell ProcessAdapter how many samples we’ll ever process in one callback
  uint32_t maxFrames = 0;
  // AAX doesn’t expose block‑size here — use a reasonable default as we us mBufferSize in AlgorithmProcessFunction.
  maxFrames = 1024;

  _processAdapter->setupProcessing(_plugin->_plugin, _plugin->_ext._params, maxFrames);

  return AAX_SUCCESS;
}

//---AAX Algorithm Callback-------------------------------------------------------------
void AAX_CALLBACK ClapAsAAX::AlgorithmProcessFunction(SClapAsAAX_Alg_Context* const inInstancesBegin[],
                                                      const void* inInstancesEnd)
{
  auto* instance = inInstancesBegin[0];
  const int32_t bufferSize = *instance->mBufferSize;
  float* const inputs[1] = {instance->mInputPP[0]};
  float* const outputs[1] = {instance->mOutputPP[0]};

  Clap::AAX::AAXProcessContext ctx{inputs, outputs, bufferSize};
  _processAdapter->process(ctx);
}

//---AAX GUI----------------------------------------------------------------------------

void ClapAsAAX::CreateViewContainer()
{
  // If the view is not created yet, create and attach it.
  if (!_wrappedView)
  {
    // Create your WrappedViewAAX instance with the CLAP plugin and gui pointers.
    _wrappedView = new WrappedViewAAX(_plugin->_plugin, _plugin->_ext._gui);

    // Call an internal attach method to initialize the container.
    // Note: 'attach' here is your helper function, not part of AAX_IViewContainer.
    _wrappedView->attach(nullptr);  // Pass a parent pointer if needed.
  }
}

void ClapAsAAX::CreateViewContents()
{
  // If additional widget setup is required beyond the initial attach,
  // delegate to a method on WrappedViewAAX.
  // For example, if your WrappedViewAAX has a method to build the widget tree:
  // _wrappedView->createWidgets();
  // Otherwise, leave empty if attach() already fully builds the GUI.
}

void ClapAsAAX::DeleteViewContainer()
{
  if (_wrappedView)
  {
    // Call your detach helper to clean up internal state and resources.
    _wrappedView->detach();

    // Delete the WrappedViewAAX instance and reset the pointer.
    delete _wrappedView;
    _wrappedView = nullptr;
  }
}

//---Clap::IHost------------------------------------------------------------------------
void ClapAsAAX::mark_dirty()
{
}
void ClapAsAAX::restartPlugin()
{
}
void ClapAsAAX::request_callback()
{
}

void ClapAsAAX::setupWrapperSpecifics(const clap_plugin_t*)
{
}
void ClapAsAAX::setupAudioBusses(const clap_plugin_t*, const clap_plugin_audio_ports_t*)
{
}
void ClapAsAAX::setupMIDIBusses(const clap_plugin_t*, const clap_plugin_note_ports_t*)
{
}
void ClapAsAAX::setupParameters(const clap_plugin_t*, const clap_plugin_params_t*)
{
}

void ClapAsAAX::param_rescan(clap_param_rescan_flags)
{
}
void ClapAsAAX::param_clear(clap_id, clap_param_clear_flags)
{
}
void ClapAsAAX::param_request_flush()
{
}

void ClapAsAAX::latency_changed()
{
}
void ClapAsAAX::tail_changed()
{
}

bool ClapAsAAX::gui_can_resize()
{
  return false;
}
bool ClapAsAAX::gui_request_resize(uint32_t, uint32_t)
{
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

bool ClapAsAAX::register_timer(uint32_t, clap_id*)
{
  return false;
}
bool ClapAsAAX::unregister_timer(clap_id)
{
  return false;
}

const char* ClapAsAAX::host_get_name()
{
  return "CLAP-as-AAX";
}

bool ClapAsAAX::supportsContextMenu() const
{
  return false;
}
bool ClapAsAAX::context_menu_populate(const clap_context_menu_target_t*,
                                      const clap_context_menu_builder_t*)
{
  return false;
}
bool ClapAsAAX::context_menu_perform(const clap_context_menu_target_t*, clap_id)
{
  return false;
}
bool ClapAsAAX::context_menu_can_popup()
{
  return false;
}
bool ClapAsAAX::context_menu_popup(const clap_context_menu_target_t*, int32_t, int32_t, int32_t)
{
  return false;
}
