#pragma once

#include <cstdint>

#include "detail/clap/fsutil.h"

namespace freeaudio::clap_wrapper::standalone
{
/*
 * Which of the standalones of one plugin running at the same time this process is.
 *
 * Every running standalone of a plugin shares one settings directory, so anything it
 * keeps per window - the window position - has to be told apart by more than the
 * plugin id. Otherwise a second instance opens exactly on top of the first, and
 * whichever quits last decides where both come back.
 *
 * The index is the lowest slot whose lock file in that directory no other process
 * holds. The lock is held for as long as this object lives, and the operating system
 * drops it when the process ends however it ends, so a crash never leaves a slot
 * taken. The first instance is 0, which is also what a standalone is when no slot can
 * be claimed at all.
 */
class InstanceSlot
{
 public:
  InstanceSlot() = default;
  ~InstanceSlot();

  InstanceSlot(const InstanceSlot &) = delete;
  InstanceSlot &operator=(const InstanceSlot &) = delete;

  // Claims the lowest free slot in `directory` on the first call; every later call
  // returns that same index, whatever directory it names.
  uint32_t claim(const fs::path &directory);

  static constexpr uint32_t maxSlots{64};

 private:
  bool claimed{false};
  uint32_t index{0};
#if defined(_WIN32)
  void *handle{nullptr};
#else
  int fd{-1};
#endif
};
}  // namespace freeaudio::clap_wrapper::standalone
