#pragma once

#include <iostream>
#include <iomanip>
#include <atomic>
#define TRACE std::cout << __FILE__ << ":" << __LINE__ << ": (" << __func__ << ")" << std::endl
#define LOG std::cout << __FILE__ << ":" << __LINE__ << ": (" << __func__ << ") "

namespace Clap::Standalone
{

// TODO uncopy this
template <typename T, uint32_t Q>
class fixedqueue
{
 public:
  inline void push(const T& val)
  {
    push(&val);
  }
  inline void push(const T* val)
  {
    _elements[_head] = *val;
    _head = (_head + 1) & Q;
  }
  inline bool pop(T& out)
  {
    if (_head == _tail)
    {
      return false;
    }
    out = _elements[_tail];
    _tail = (_tail + 1) & Q;
    return true;
  }

 private:
  T _elements[Q] = {};
  std::atomic_uint32_t _head = 0u;
  std::atomic_uint32_t _tail = 0u;

  static_assert((Q & (Q - 1)) == 0, "Q needs to be a multiple of 2");
};
}  // namespace Clap::Standalone
