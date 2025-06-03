#pragma once

/*
 * this is a shared header to connect the wrapped view with the actual AU
 *
 * the connection will be created when the wrapped View (cocoa) is asked to connect
 * via the uiViewForAudioUnit call which provides an instance of the audiounit component.
 * this (private) property is being asked then to know the actual instance the view
 * is connected to.
 *
 * With the hinting, the view can access all necessary structures in the
 * audiounit connected.
 */

#include <iostream>
#include <functional>
#include "clap_proxy.h"
#include <AudioToolbox/AudioUnitProperties.h>

static const AudioUnitPropertyID kAudioUnitProperty_ClapWrapper_UIConnection_id = 0x00010911;

namespace free_audio::auv2_wrapper
{

// this struct is set up from the plugin for the view.
//
typedef struct ui_connection
{
  uint32_t identifier = kAudioUnitProperty_ClapWrapper_UIConnection_id;
  Clap::Plugin *_plugin = nullptr;   // points to the plugin instance
  clap_window_t *_window = nullptr;  // points to a window handle, actually ptr to wrapping NSView class
  uint32_t *_canary = nullptr;       // a canary in the Windows class
  std::function<void(clap_window_t *, uint32_t *)> _registerWindow = nullptr;
  std::function<void()> _createWindow = nullptr;
  std::function<void()> _destroyWindow = nullptr;
} ui_connection;

}  // namespace free_audio::auv2_wrapper
