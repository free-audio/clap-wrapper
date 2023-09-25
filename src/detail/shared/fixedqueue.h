#pragma once

#include <cstdint>
#include <atomic>

namespace ClapWrapper::detail::shared
{

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
    _head = (_head + 1) & _wrapMask;
  }
  inline bool pop(T& out)
  {
    if (_head == _tail)
    {
      return false;
    }
    out = _elements[_tail];
    _tail = (_tail + 1) & _wrapMask;
    return true;
  }

 private:
  T _elements[Q] = {};
  std::atomic_uint32_t _head = 0u;
  std::atomic_uint32_t _tail = 0u;

  static constexpr uint32_t _wrapMask = Q - 1;
  static_assert((Q & _wrapMask) == 0, "Q needs to be a multiple of 2");
};
}  // namespace ClapWrapper::detail::shared
