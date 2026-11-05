
#include <clap/ext/gui.h>
#include <cstdint>
#include <functional>
#include <iostream>

#include <Cocoa/Cocoa.h>

namespace free_audio::auv2_wrapper
{
bool auv2shared_mm_request_resize(const clap_window_t *win, uint32_t w, uint32_t h)
{
  if (!win) return false;

  auto *nsv = (NSView *)win;
  [nsv setFrame:NSMakeRect(0, 0, w, h)];

  return true;
}

// The real main thread, which is what AppKit means by it. Plugin::is_main_thread()
// is not the same question: it also answers true under AlwaysMainThread(), on
// whatever thread the host happened to call in on.
bool auv2shared_mm_is_main_thread()
{
  return [NSThread isMainThread];
}

void auv2shared_mm_run_on_main_thread(const std::function<void()> &f)
{
  if ([NSThread isMainThread])
  {
    f();
    return;
  }
  const auto *fn = &f;
  dispatch_sync(dispatch_get_main_queue(), ^{
    (*fn)();
  });
}
}  // namespace free_audio::auv2_wrapper
