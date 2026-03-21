// the wrapped plugview

#include "clap/clap.h"
#include "AAX_CEffectGUI.h"

AAX_IEffectGUI *AAX_CALLBACK Wrapped_AAX_GUI_Create(void);

class ClapAsAAX;

class Wrapped_AAX_GUI : public AAX_CEffectGUI
{
 public:
  Wrapped_AAX_GUI();
  ~Wrapped_AAX_GUI() AAX_OVERRIDE;

  bool setWindowSize(uint32_t width, uint32_t height);

 protected:
  // AAX_CEffectGUI
  void CreateViewContents() AAX_OVERRIDE;
  void CreateViewContainer() AAX_OVERRIDE;
  void DeleteViewContainer() AAX_OVERRIDE;

  AAX_Result GetViewSize(AAX_Point *oEffectViewSize) const AAX_OVERRIDE;

  AAX_Result TimerWakeup() AAX_OVERRIDE;

  // Wrapped_AAX_GUI
  virtual void CreateEffectView(void *inSystemWindow);

 protected:
  ClapAsAAX *_clap = nullptr;
  const clap_plugin_t *_plugin = nullptr;
  const clap_plugin_gui_t *_gui = nullptr;
  clap_window_t _platformwindow = {nullptr, {(void *)nullptr}};
  bool _created = false;
  bool _resizeInTimer = false;
};
