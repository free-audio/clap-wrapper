
#pragma once
#include <atomic>
namespace ClapWrapper::detail::shared
{
struct SpinLock
{
 private:
  std::atomic<bool> locked_{false};

 public:
  SpinLock() : locked_(false)
  {
  }

  void lock()
  {
    while (locked_.exchange(true, std::memory_order_acquire))
    {
    }
  }

  // for a caller that must not wait -- with lock()/unlock() this also makes
  // std::unique_lock<SpinLock>(lock, std::try_to_lock) work
  bool try_lock()
  {
    return !locked_.exchange(true, std::memory_order_acquire);
  }

  void unlock()
  {
    locked_.store(false, std::memory_order_release);
  }
};

struct SpinLockGuard
{
  SpinLock &lock;
  SpinLockGuard(SpinLock &l) : lock(l)
  {
    lock.lock();
  };
  ~SpinLockGuard()
  {
    lock.unlock();
  }
};
}  // namespace ClapWrapper::detail::shared