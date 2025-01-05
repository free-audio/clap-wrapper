
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