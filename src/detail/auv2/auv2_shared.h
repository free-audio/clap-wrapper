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
#include <memory>
#include <AudioUnitSDK/AUUtility.h>
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
  Clap::Plugin *_plugin = nullptr;  // points to the plugin instance
  /// Serializes host lifecycle calls with editor calls and outlives either connection endpoint.
  std::shared_ptr<ausdk::AUMutex> _mainThreadMutex;
  clap_window_t *_window = nullptr;  // points to a window handle, actually ptr to wrapping NSView class
  uint32_t *_canary = nullptr;       // a canary in the Windows class
  std::function<void(clap_window_t *, uint32_t *)> _registerWindow = nullptr;
  std::function<void()> _createWindow = nullptr;
  std::function<void()> _destroyWindow = nullptr;
} ui_connection;

// Implemented in auv2_shared.mm: hands a size the plugin asked for to the Cocoa
// view behind `win`. Main thread only - it messages AppKit, and the view calls
// back into the plugin. \see WrapAsAUV2::onIdle()
bool auv2shared_mm_request_resize(const clap_window_t *win, uint32_t width, uint32_t height);

}  // namespace free_audio::auv2_wrapper

#ifdef __OBJC__
#import <Foundation/Foundation.h>

/*
 * The wrapper's NSView subclass is generated per plugin under a process-wide
 * unique class name (see wrappedview.mm), so no other translation unit can name
 * the type. This protocol is the one thing they need from it.
 *
 * It is only ever used as a compile time cast target - the call site asks
 * -respondsToSelector: instead of -conformsToProtocol: - so two wrapped plugins
 * in one process do not have to agree on whose copy of it the ObjC runtime
 * registered.
 */
@protocol ClapWrapperAUv2ResizableView <NSObject>
// Applies a size the plugin requested without echoing it back into set_size().
- (BOOL)clapWrapperRequestResizeToWidth:(uint32_t)width height:(uint32_t)height;
@end
#endif
