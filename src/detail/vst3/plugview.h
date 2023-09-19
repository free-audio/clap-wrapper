#pragma once

/*
    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

*/

#include "base/source/fobject.h"
#include <pluginterfaces/gui/iplugview.h>
#include <clap/clap.h>
#include <functional>

using namespace Steinberg;

class WrappedView : public Steinberg::IPlugView, public Steinberg::FObject
{
 public:
  WrappedView(const clap_plugin_t* plugin, const clap_plugin_gui_t* gui, std::function<void()> onDestroy,
              std::function<void()> onRunLoopAvailable);
  ~WrappedView();

  // IPlugView interface
  tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override;

  /** The parent window of the view has been created, the (platform) representation of the view
		should now be created as well.
			Note that the parent is owned by the caller and you are not allowed to alter it in any way
		other than adding your own views.
			Note that in this call the plug-in could call a IPlugFrame::resizeView ()!
			\param parent : platform handle of the parent window or view
			\param type : \ref platformUIType which should be created */
  tresult PLUGIN_API attached(void* parent, FIDString type) override;

  /** The parent window of the view is about to be destroyed.
			You have to remove all your own views from the parent window or view. */
  tresult PLUGIN_API removed() override;

  /** Handling of mouse wheel. */
  tresult PLUGIN_API onWheel(float distance) override;

  /** Handling of keyboard events : Key Down.
			\param key : unicode code of key
			\param keyCode : virtual keycode for non ascii keys - see \ref VirtualKeyCodes in keycodes.h
			\param modifiers : any combination of modifiers - see \ref KeyModifier in keycodes.h
			\return kResultTrue if the key is handled, otherwise kResultFalse. \n
							<b> Please note that kResultTrue must only be returned if the key has really been
		 handled. </b> Otherwise key command handling of the host might be blocked! */
  tresult PLUGIN_API onKeyDown(char16 key, int16 keyCode, int16 modifiers) override;

  /** Handling of keyboard events : Key Up.
			\param key : unicode code of key
			\param keyCode : virtual keycode for non ascii keys - see \ref VirtualKeyCodes in keycodes.h
			\param modifiers : any combination of KeyModifier - see \ref KeyModifier in keycodes.h
			\return kResultTrue if the key is handled, otherwise return kResultFalse. */
  tresult PLUGIN_API onKeyUp(char16 key, int16 keyCode, int16 modifiers) override;

  /** Returns the size of the platform representation of the view. */
  tresult PLUGIN_API getSize(ViewRect* size) override;

  /** Resizes the platform representation of the view to the given rect. Note that if the plug-in
	 *	requests a resize (IPlugFrame::resizeView ()) onSize has to be called afterward. */
  tresult PLUGIN_API onSize(ViewRect* newSize) override;

  /** Focus changed message. */
  tresult PLUGIN_API onFocus(TBool state) override;

  /** Sets IPlugFrame object to allow the plug-in to inform the host about resizing. */
  tresult PLUGIN_API setFrame(IPlugFrame* frame) override;

  /** Is view sizable by user. */
  tresult PLUGIN_API canResize() override;

  /** On live resize this is called to check if the view can be resized to the given rect, if not
	 *	adjust the rect to the allowed size. */
  tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override;

  //---Interface------
  OBJ_METHODS(WrappedView, FObject)
  DEFINE_INTERFACES
  DEF_INTERFACE(IPlugView)
  END_DEFINE_INTERFACES(FObject)
  REFCOUNT_METHODS(FObject)

  // wrapper needed interfaces
  bool request_resize(uint32_t width, uint32_t height);

 private:
  void ensure_ui();
  void drop_ui();
  const clap_plugin_t* _plugin = nullptr;
  const clap_plugin_gui_t* _extgui = nullptr;
  std::function<void()> _onDestroy = nullptr, _onRunLoopAvailable = nullptr;
  clap_window_t _window = {nullptr, {nullptr}};
  IPlugFrame* _plugFrame = nullptr;
  ViewRect _rect = {0, 0, 0, 0};
  bool _created = false;
  bool _attached = false;

#if LIN
 public:
  Steinberg::Linux::IRunLoop* getRunLoop()
  {
    return _runLoop;
  }

 private:
  Steinberg::Linux::IRunLoop* _runLoop = nullptr;
#endif
};
