#include "plugview.h"
#include <clap/clap.h>
#include <cassert>
#include <iostream>

WrappedView::WrappedView(const clap_plugin_t* plugin, const clap_plugin_gui_t* gui,
                         std::function<void()> onReleaseAdditionalReferences,
                         std::function<void(bool)> onDestroy, std::function<void()> onRunLoopAvailable)
  : IPlugView()
  , FObject()
  , _plugin(plugin)
  , _extgui(gui)
  , _onReleaseAdditionalReferences(onReleaseAdditionalReferences)
  , _onRunLoopAvailable(onRunLoopAvailable)
  , _onDestroy(onDestroy)
{
}

WrappedView::~WrappedView()
{
  drop_ui();
}

void WrappedView::ensure_ui()
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
#if LIN
    api = CLAP_WINDOW_API_X11;
#endif

    if (_extgui->is_api_supported(_plugin, api, false)) _extgui->create(_plugin, api, false);

    _created = true;
  }
}

void WrappedView::drop_ui()
{
  if (_created)
  {
    releaseAdditionalReferences();
    _attached = false;
    if (_onDestroy)
    {
      _onDestroy(true);
    }
    _extgui->destroy(_plugin);
    _created = false;
  }
  else
  {
    if (_onDestroy)
    {
      _onDestroy(false);
    }
  }
}

void WrappedView::releaseAdditionalReferences()
{
  // releases things like IContextMenu when the wrapper provided entries, but the user chose
  // a plugin-owned entry.

  if (_onReleaseAdditionalReferences)
  {
    _onReleaseAdditionalReferences();
  }
}

tresult PLUGIN_API WrappedView::isPlatformTypeSupported(FIDString type)
{
  static struct vst3_and_clap_match_types_t
  {
    const char* VST3;
    const char* CLAP;
  } platformTypeMatches[] = {{kPlatformTypeHWND, CLAP_WINDOW_API_WIN32},
                             {kPlatformTypeNSView, CLAP_WINDOW_API_COCOA},
                             {kPlatformTypeX11EmbedWindowID, CLAP_WINDOW_API_X11},
                             {nullptr, nullptr}};
  auto* n = platformTypeMatches;
  while (n->VST3 && n->CLAP)
  {
    if (!strcmp(type, n->VST3))
    {
      if (_extgui->is_api_supported(_plugin, n->CLAP, false))
      {
        return kResultOk;
      }
    }
    ++n;
  }

  return kResultFalse;
}

tresult PLUGIN_API WrappedView::attached(void* parent, FIDString /*type*/)
{
#if WIN
  _window = {CLAP_WINDOW_API_WIN32, {parent}};
#endif

#if MAC
  _window = {CLAP_WINDOW_API_COCOA, {parent}};
#endif

#if LIN
  _window = {CLAP_WINDOW_API_X11, {parent}};
#endif

  ensure_ui();
  _extgui->set_parent(_plugin, &_window);
  _attached = true;
  if (_extgui->can_resize(_plugin))
  {
    uint32_t w = _rect.getWidth();
    uint32_t h = _rect.getHeight();
    if (_extgui->adjust_size(_plugin, &w, &h))
    {
      _rect.right = _rect.left + w + 1;
      _rect.bottom = _rect.top + h + 1;
    }
    _extgui->set_size(_plugin, w, h);
  }
  _extgui->show(_plugin);
  return kResultOk;
}

tresult PLUGIN_API WrappedView::removed()
{
  releaseAdditionalReferences();
  _attached = false;
  _window.ptr = nullptr;
  return kResultOk;
}

tresult PLUGIN_API WrappedView::onWheel(float /*distance*/)
{
  return kResultFalse;
}

tresult PLUGIN_API WrappedView::onKeyDown(char16 /*key*/, int16 /*keyCode*/, int16 /*modifiers*/)
{
  return kResultFalse;
}

tresult PLUGIN_API WrappedView::onKeyUp(char16 /*key*/, int16 /*keyCode*/, int16 /*modifiers*/)
{
  return kResultFalse;
}

tresult PLUGIN_API WrappedView::getSize(ViewRect* size)
{
  ensure_ui();
  if (size)
  {
    uint32_t w, h;
    if (_extgui->get_size(_plugin, &w, &h))
    {
      size->right = size->left + w;
      size->bottom = size->top + h;
      _rect = *size;
      return kResultOk;
    }
    return kResultFalse;
  }
  return kInvalidArgument;
}

tresult PLUGIN_API WrappedView::onSize(ViewRect* newSize)
{
  // TODO: discussion took place if this call should be ignored completely
  // since it seems not to match the CLAP UI scheme.
  // for now, it is left in and might be removed in the future.
  if (!newSize) return kResultFalse;

  _rect = *newSize;
  if (_created && _attached)
  {
    if (_extgui->can_resize(_plugin))
    {
      uint32_t w = _rect.getWidth();
      uint32_t h = _rect.getHeight();
      if (_extgui->adjust_size(_plugin, &w, &h))
      {
        _rect.right = _rect.left + w;
        _rect.bottom = _rect.top + h;
      }
      if (_extgui->set_size(_plugin, w, h))
      {
        return kResultOk;
      }
    }
    else
    {
      return kResultFalse;
    }
  }
  return kResultOk;
}

tresult PLUGIN_API WrappedView::onFocus(TBool state)
{
  // TODO: this might be something for the wrapperhost API
  // to notify the plugin about a focus change
  if (state == false)
  {
    releaseAdditionalReferences();
  }
  return kResultOk;
}

tresult PLUGIN_API WrappedView::setFrame(IPlugFrame* frame)
{
  releaseAdditionalReferences();

  _plugFrame = frame;

#if LIN
  if (_plugFrame)
  {
    if (_plugFrame->queryInterface(Steinberg::Linux::IRunLoop::iid, (void**)&_runLoop) ==
            Steinberg::kResultOk &&
        _onRunLoopAvailable)
    {
      _onRunLoopAvailable();
    }
  }
#endif
  return kResultOk;
}

tresult PLUGIN_API WrappedView::canResize()
{
  ensure_ui();
  return _extgui->can_resize(_plugin) ? kResultOk : kResultFalse;
}

tresult PLUGIN_API WrappedView::checkSizeConstraint(ViewRect* rect)
{
  ensure_ui();
  uint32_t w = rect->getWidth();
  uint32_t h = rect->getHeight();
  if (_extgui->adjust_size(_plugin, &w, &h))
  {
    rect->right = rect->left + w;
    rect->bottom = rect->top + h;
    return kResultOk;
  }
  return kResultFalse;
}

bool WrappedView::request_resize(uint32_t width, uint32_t height)
{
  auto oldrect = _rect;
  _rect.right = _rect.left + (int32)width;
  _rect.bottom = _rect.top + (int32)height;

  if (_plugFrame && !_plugFrame->resizeView(this, &_rect))
  {
    _rect = oldrect;
    return false;
  }
  return true;
}
tresult WrappedView::setContentScaleFactor(IPlugViewContentScaleSupport::ScaleFactor factor)
{
  ensure_ui();
  if (_extgui->set_scale(_plugin, factor))
  {
    return kResultOk;
  }
  return kResultFalse;
}
