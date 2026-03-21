#pragma once

// utilities that are needed all over the different code parts

// #include "AAX.h"
#include "clap/clap.h"
#include <vector>
#include <string>
#include <atomic>
#include <cstdint>

std::string createAAXId(clap_id id);
uint32_t AAXIDfromString(const char *str);
uint32_t AAXIDfromString(const std::string &str);
std::vector<std::string> generateShortStrings(const std::string &input);

#pragma once
#include <atomic>
#include <cstdint>

struct ParamChange
{
  clap_id paramID;  // we are using the clap_id
  double value;     // applied to clap value range
  void *cookie;     // cookie from param_info
};

class ParamChangeQueue
{
 public:
  ParamChangeQueue() : _buffer(nullptr), _capacity(0), _indexMask(0), _writeIndex(0), _readIndex(0)
  {
  }

  ~ParamChangeQueue()
  {
    delete[] _buffer;
  }

  // Must be called before first use
  bool init(size_t capacity)
  {
    if (capacity < 2) return false;

    // Round up to next power of two
    size_t pow2 = 1;
    while (pow2 < capacity) pow2 <<= 1;

    _capacity = pow2;
    _indexMask = pow2 - 1;

    _buffer = new ParamChange[_capacity];

    if (!_buffer) return false;

    _writeIndex.store(0, std::memory_order_relaxed);
    _readIndex.store(0, std::memory_order_relaxed);

    return true;
  }

  // Producer thread (Control thread)
  bool push(const ParamChange &change)
  {
    const uint64_t write = _writeIndex.load(std::memory_order_relaxed);
    const uint64_t nextWrite = write + 1;

    // Full? (one slot is always left empty)
    if (nextWrite - _readIndex.load(std::memory_order_acquire) > _capacity) return false;

    _buffer[write & _indexMask] = change;
    _writeIndex.store(nextWrite, std::memory_order_release);
    return true;
  }

  // Consumer thread (Audio thread)
  bool pop(ParamChange &outChange)
  {
    const uint64_t read = _readIndex.load(std::memory_order_relaxed);

    if (read == _writeIndex.load(std::memory_order_acquire)) return false;  // empty

    outChange = _buffer[read & _indexMask];
    _readIndex.store(read + 1, std::memory_order_release);
    return true;
  }

  // Optional: clear queue (not realtime-safe)
  void clear()
  {
    _readIndex.store(_writeIndex.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }

 private:
  ParamChange *_buffer = nullptr;
  size_t _capacity;
  size_t _indexMask;

  std::atomic<uint64_t> _writeIndex;
  std::atomic<uint64_t> _readIndex;
};
