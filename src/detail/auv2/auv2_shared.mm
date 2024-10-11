
#include <clap/ext/gui.h>
#include <cstdint>
#include <iostream>

#include <Cocoa/Cocoa.h>

namespace free_audio::auv2_wrapper
{
bool auv2shared_mm_request_resize(const clap_window_t* win, uint32_t w, uint32_t h)
{
  if (!win) return false;

  auto* nsv = (NSView*)win;
  [nsv setFrameSize:NSMakeSize(w, h)];

  return false;
}
}  // namespace free_audio::auv2_wrapper
