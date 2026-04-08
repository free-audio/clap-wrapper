//

#include "plugview.h"
#include "wrapper.h"

#include "AAX_IViewContainer.h"
#include "AAX_CAutoreleasePool.h"

AAX_IEffectGUI *AAX_CALLBACK Wrapped_AAX_GUI_Create(void)
{
  return new Wrapped_AAX_GUI;
}

Wrapped_AAX_GUI::Wrapped_AAX_GUI()
{
  // init if necessary
}
Wrapped_AAX_GUI::~Wrapped_AAX_GUI()
{
  AAX_CAutoreleasePool autorelease;
  // shutdown UI
  if (_gui && _created)
  {
    _gui->destroy(_plugin);
  }
  if (_clap)
  {
    _clap->_aax_view = nullptr;
    _clap = nullptr;
  }
  _gui = nullptr;
  _plugin = nullptr;
}

void Wrapped_AAX_GUI::CreateViewContents()
{
  // loading views and resources
  // this is what the actual CLAP is doing
}

void Wrapped_AAX_GUI::CreateEffectView(void *inSystemWindow)
{
#ifdef WIN32
#define CLAP_WINDOW_API CLAP_WINDOW_API_WIN32;
#elif MAC
#define CLAP_WINDOW_API CLAP_WINDOW_API_COCOA;
#else
#error I don't think we belong here
#endif
  _platformwindow.api = CLAP_WINDOW_API;
  _platformwindow.ptr = inSystemWindow;
#undef CLAP_WINDOW_API

  auto params = this->GetEffectParameters();
  _clap = dynamic_cast<ClapAsAAX *>(params);

  if (_clap)
  {
    // introduce this object to the associated wrapper instance
    _clap->_aax_view = this;

    _gui = _clap->_plugin->_ext._gui;
    _plugin = _clap->_plugin->_plugin;

    if (_gui->create(_plugin, _platformwindow.api, false))
    {
      _created = true;
      _gui->set_parent(_plugin, &_platformwindow);
      // _gui->set_scale(_plugin, 1.0);
      // Protools on Windows does not support hidpi today:
      // https://kb.avid.com/pkb/articles/en_US/Knowledge/Pro-Tools-and-4K-Resolution-Monitors-on-Windows
    }
  }
}

void Wrapped_AAX_GUI::CreateViewContainer()
{
  if (this->GetViewContainerType() == AAX_eViewContainer_Type_HWND ||
      this->GetViewContainerType() == AAX_eViewContainer_Type_NSView)
  {
    this->CreateEffectView(this->GetViewContainerPtr());
  }
}

void Wrapped_AAX_GUI::DeleteViewContainer()
{
  AAX_CAutoreleasePool autorelease;
  if (_clap)
  {
    _clap->_aax_view = nullptr;
    _clap = nullptr;
  }
  if (_gui && _created)
  {
    _gui->destroy(_plugin);
    _created = false;
    _gui = nullptr;
    _plugin = nullptr;
  }
}

AAX_Result Wrapped_AAX_GUI::GetViewSize(AAX_Point *oEffectViewSize) const
{
  uint32_t w, h;
  if (_gui->get_size(_plugin, &w, &h))
  {
    // AAX wants the size in float, which is really awkward
    oEffectViewSize->horz = (float)w;
    oEffectViewSize->vert = (float)h;
  }
  else
  {
    oEffectViewSize->horz = 400.f;
    oEffectViewSize->vert = 300.f;
  }

  return AAX_SUCCESS;
}

bool Wrapped_AAX_GUI::setWindowSize(uint32_t width, uint32_t height)
{
  if (!_created)
  {
    _resizeInTimer = true;
  }
  auto *vc = GetViewContainer();
  if (vc)
  {
    AAX_Point p((float)height, (float)width);  // yes, on AAX everything is upside down
    return (vc->SetViewSize(p) == AAX_SUCCESS);
  }
  return false;
}

AAX_Result Wrapped_AAX_GUI::TimerWakeup()
{
  if (_resizeInTimer)
  {
    _resizeInTimer = false;
    AAX_Point size;
    if (GetViewSize(&size) == AAX_SUCCESS)
    {
      _resizeInTimer = !setWindowSize(size.horz, size.vert);
    }
  }
  return AAX_CEffectGUI::TimerWakeup();
}