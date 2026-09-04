#pragma once

#include <cstdint>
#include <atomic>

namespace ClapWrapper::detail::shared
{

template <typename T, uint32_t Q>
class fixedqueue
{
 public:
  inline void push(const T &val)
  {
    push(&val);
  }
  inline void push(const T *val)
  {
    _elements[_head] = *val;
    _head = (_head + 1) & _wrapMask;
  }
  inline bool try_push(const T &val)
  {
    return try_push(&val);
  }
  inline bool try_push(const T *val)
  {
    const auto head = _head.load(std::memory_order_relaxed);
    const auto next = (head + 1) & _wrapMask;
    if (next == _tail.load(std::memory_order_acquire))
    {
      return false;
    }
    _elements[head] = *val;
    _head.store(next, std::memory_order_release);
    return true;
  }
  inline bool pop(T &out)
  {
    const auto tail = _tail.load(std::memory_order_relaxed);
    if (_head.load(std::memory_order_acquire) == tail)
    {
      return false;
    }
    out = _elements[tail];
    _tail.store((tail + 1) & _wrapMask, std::memory_order_release);
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
