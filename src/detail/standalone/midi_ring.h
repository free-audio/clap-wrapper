#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace freeaudio::clap_wrapper::standalone
{
/*
 * Whole MIDI messages, short ones and sysex alike, in the order they came, between one
 * producer and one consumer, without a lock or an allocation. A message is stored as its
 * length and its bytes; one that does not fit is refused whole, never split.
 */
template <uint32_t Size>
class MidiRing
{
  static_assert((Size & (Size - 1)) == 0, "Size needs to be a power of two");

 public:
  // the largest message that fits at all
  static constexpr uint32_t maxMessage{Size - sizeof(uint32_t)};

  // false, and nothing written, if the message does not fit now
  bool tryPush(const uint8_t *data, uint32_t size)
  {
    const uint32_t need{size + static_cast<uint32_t>(sizeof(uint32_t))};
    if (!data || size == 0 || size > maxMessage) return false;

    const auto head{_head.load(std::memory_order_relaxed)};
    const auto tail{_tail.load(std::memory_order_acquire)};
    if (Size - (head - tail) < need) return false;

    copyIn(head, &size, sizeof(size));
    copyIn(head + sizeof(size), data, size);
    _head.store(head + need, std::memory_order_release);
    return true;
  }

  // the size of the next message; 0 if there is none
  uint32_t nextSize() const
  {
    const auto tail{_tail.load(std::memory_order_relaxed)};
    if (_head.load(std::memory_order_acquire) == tail) return 0;
    uint32_t size{0};
    copyOut(tail, &size, sizeof(size));
    return size;
  }

  // the first byte of the next message, its status; 0 if there is none
  uint8_t nextStatus() const
  {
    if (nextSize() == 0) return 0;
    uint8_t status{0};
    copyOut(_tail.load(std::memory_order_relaxed) + sizeof(uint32_t), &status, 1);
    return status;
  }

  // the next message into out, which holds at least nextSize() bytes; its size, 0 if none
  uint32_t pop(uint8_t *out)
  {
    const auto size{nextSize()};
    if (size == 0) return 0;
    const auto tail{_tail.load(std::memory_order_relaxed)};
    copyOut(tail + sizeof(size), out, size);
    _tail.store(tail + static_cast<uint32_t>(sizeof(size)) + size, std::memory_order_release);
    return size;
  }

  // drops everything; for the consumer
  void clear()
  {
    _tail.store(_head.load(std::memory_order_acquire), std::memory_order_release);
  }

 private:
  void copyIn(uint32_t at, const void *from, uint32_t size)
  {
    const auto offset{at & (Size - 1)};
    const auto first{std::min(size, Size - offset)};
    memcpy(_bytes + offset, from, first);
    memcpy(_bytes, static_cast<const uint8_t *>(from) + first, size - first);
  }

  void copyOut(uint32_t at, void *to, uint32_t size) const
  {
    const auto offset{at & (Size - 1)};
    const auto first{std::min(size, Size - offset)};
    memcpy(to, _bytes + offset, first);
    memcpy(static_cast<uint8_t *>(to) + first, _bytes, size - first);
  }

  uint8_t _bytes[Size]{};
  // free-running; the difference is what is stored
  std::atomic<uint32_t> _head{0}, _tail{0};
};
}  // namespace freeaudio::clap_wrapper::standalone
