#ifndef PLUGVIEW_AAX_H
#define PLUGVIEW_AAX_H

#include <cstdint>
#include "AAX_IViewContainer.h"

// Forward declaration of the AAX effect wrapper
class ClapAsAAX;

// WrappedViewAAX provides an AAX-specific view wrapper interface.
// It encapsulates the creation, attachment, and management of the plugin's GUI.
class WrappedViewAAX : public AAX_IViewContainer
{
 public:
  WrappedViewAAX(const clap_plugin_t* plugin, const clap_plugin_gui_t* gui);

  ~WrappedViewAAX();

  int32_t GetType();
  void* GetPtr();
  AAX_Result GetModifiers(uint32_t* outModifiers);
  AAX_Result SetViewSize(AAX_Point& inSize);
  AAX_Result HandleParameterMouseDown(AAX_CParamID inParamID, uint32_t inModifiers);
  AAX_Result HandleParameterMouseDrag(AAX_CParamID inParamID, uint32_t inModifiers);
  AAX_Result HandleParameterMouseUp(AAX_CParamID inParamID, uint32_t inModifiers);
  AAX_Result HandleParameterMouseEnter(AAX_CParamID inParamID, uint32_t inModifiers);
  AAX_Result HandleParameterMouseExit(AAX_CParamID inParamID, uint32_t inModifiers);
  AAX_Result HandleMultipleParametersMouseDown(const AAX_CParamID* inParamIDs, uint32_t inNumOfParams,
                                               uint32_t inModifiers);
  AAX_Result HandleMultipleParametersMouseDrag(const AAX_CParamID* inParamIDs, uint32_t inNumOfParams,
                                               uint32_t inModifiers);
  AAX_Result HandleMultipleParametersMouseUp(const AAX_CParamID* inParamIDs, uint32_t inNumOfParams,
                                             uint32_t inModifiers);
  bool attach(void* parent);
  void detach();

 private:
  void ensure_ui();
  const clap_plugin_t* _plugin = nullptr;
  const clap_plugin_gui_t* _extgui = nullptr;
  clap_window_t _window = {nullptr, {nullptr}};
  bool _created = false;
  bool _attached;
};

#endif  // PLUGVIEW_AAX_H
