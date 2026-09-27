#include <clap/ext/gui.h>
#include <cstdint>
#include <iostream>

#include <Cocoa/Cocoa.h>

#include "auv2_shared.h"

namespace free_audio::auv2_wrapper
{
bool auv2shared_mm_request_resize(const clap_window_t *win, uint32_t w, uint32_t h)
{
  if (!win) return false;

  // _window carries the NSView itself - that is the AUv2 contract between the
  // audio unit and its generated view class, not an oversight.
  auto *nsv = (NSView *)win;

  // The class is generated per plugin, so it can only be reached through the
  // protocol. A view that does not answer to it is not one of ours.
  if (![nsv respondsToSelector:@selector(clapWrapperRequestResizeToWidth:height:)]) return false;

  return [(NSView<ClapWrapperAUv2ResizableView> *)nsv clapWrapperRequestResizeToWidth:w height:h];
}
}  // namespace free_audio::auv2_wrapper
