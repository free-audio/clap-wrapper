#include <clap/clap.h>
#include "plugview.h"
#include "wrapasaax.h"
#include <cassert>

// Constructor: Takes a pointer to the AAX effect wrapper
WrappedViewAAX::WrappedViewAAX(const clap_plugin_t* plugin, const clap_plugin_gui_t* gui)
  : AAX_IViewContainer(), _plugin(plugin), _extgui(gui)
{
}

// Destructor: Ensures the view is detached and cleaned up
WrappedViewAAX::~WrappedViewAAX()
{
  if (_attached)
  {
    // TODO: Ensure the view is detached and cleaned up
    _attached = false;
  }
}

//---AAX_IViewContainer-------------------------------------------------------------------

int32_t WrappedViewAAX::GetType()
{
  return 0;
}

void* WrappedViewAAX::GetPtr()
{
  return nullptr;
}

AAX_Result WrappedViewAAX::GetModifiers(uint32_t* outModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::SetViewSize(AAX_Point& inSize)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleParameterMouseDown(AAX_CParamID inParamID, uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleParameterMouseDrag(AAX_CParamID inParamID, uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleParameterMouseUp(AAX_CParamID inParamID, uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleParameterMouseEnter(AAX_CParamID inParamID, uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleParameterMouseExit(AAX_CParamID inParamID, uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleMultipleParametersMouseDown(const AAX_CParamID* inParamIDs,
                                                             uint32_t inNumOfParams,
                                                             uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleMultipleParametersMouseDrag(const AAX_CParamID* inParamIDs,
                                                             uint32_t inNumOfParams,
                                                             uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

AAX_Result WrappedViewAAX::HandleMultipleParametersMouseUp(const AAX_CParamID* inParamIDs,
                                                           uint32_t inNumOfParams, uint32_t inModifiers)
{
  return AAX_SUCCESS;
}

//---Helper methods-----------------------------------------------------------------------

void WrappedViewAAX::ensure_ui()
{
  if (!_created)
  {
    const char* api{nullptr};
#if MAC
    api = CLAP_WINDOW_API_COCOA;
#endif
#if WIN
    api = CLAP_WINDOW_API_WIN32;
#endif

    if (_extgui->is_api_supported(_plugin, api, false)) _extgui->create(_plugin, api, false);

    _created = true;
  }
}

bool WrappedViewAAX::attach(void* parent)
{
  // Determine the API string based on the platform
  const char* api = nullptr;
#if WIN
  _window = {CLAP_WINDOW_API_WIN32, {parent}};
#endif

#if MAC
  _window = {CLAP_WINDOW_API_COCOA, {parent}};
#endif

#if LIN
  _window = {CLAP_WINDOW_API_X11, {parent}};
#endif

  // Create a CLAP window structure to pass to the GUI extension
  clap_window_t window = {};
  window.api = api;
  window.ptr = parent;  // This is the native parent window handle

  // Ensure the UI is created (this might call the CLAP GUI create() function)
  ensure_ui();

  // Set the parent for the GUI using the CLAP extension
  if (_extgui && _extgui->set_parent)
  {
    _extgui->set_parent(_plugin, &window);
  }

  // Optionally adjust the size if supported
  if (_extgui && _extgui->can_resize && _extgui->can_resize(_plugin))
  {
    // You might query a preferred size and then adjust
    uint32_t width = 400, height = 300;
    if (_extgui->adjust_size)
    {
      _extgui->adjust_size(_plugin, &width, &height);
    }
    // Set the size using your UI logic if needed (this might involve calling your internal SetViewSize)
  }

  // Show the GUI (this call may be required by the CLAP GUI extension)
  if (_extgui && _extgui->show)
  {
    _extgui->show(_plugin);
  }

  _attached = true;
  return true;
}

void WrappedViewAAX::detach()
{
  if (_attached)
  {
    // If your CLAP GUI extension provides a destroy() function, call it
    if (_extgui && _extgui->destroy)
    {
      _extgui->destroy(_plugin);
    }

    // Perform any additional cleanup for your AAX_IViewContainer here.
    // For example, release any UI elements or deallocate resources.

    _attached = false;
  }
}
