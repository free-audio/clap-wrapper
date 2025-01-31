#pragma once

/*
    a minimalistic OS layer
*/

#include <atomic>
#include <string>
#include <functional>
#define FMT_HEADER_ONLY 1
#include "fmt/format.h"
#include "fmt/ranges.h"

namespace os
{

class State
{
  // the State class ensures that a specific function pair (like init/terminate) is only called in pairs.
  // the bool _state reflects if the _on lambda has already been called
  // on destruction, it makes sure that the _off lambda is definitely called.
  //
  // the construct should only be applied to when no state machine can cover this properly
  // or your own code depends on correct calling sequences of an external code base and should
  // help to implement defensive programming.

 public:
  State(std::function<void()> on, std::function<void()> off) : _on(on), _off(off)
  {
  }
  ~State()
  {
    off();
  }
  void on()
  {
    if (!_state)
    {
      _on();
    }
    _state = true;
  }
  void off()
  {
    if (_state)
    {
      _off();
    }
    _state = false;
  }

 private:
  bool _state = false;
  std::function<void()> _on, _off;
};

class IPlugObject
{
 public:
  virtual void onIdle() = 0;
  virtual ~IPlugObject()
  {
  }
};
void attach(IPlugObject* plugobject);
void detach(IPlugObject* plugobject);
uint64_t getTickInMS();
std::string getModulePath();
std::string getParentFolderName();
std::string getBinaryName();

void log(const char* text);

template <typename... Args>
void log(fmt::string_view format_str, Args&&... args)
{
  fmt::memory_buffer buf;
  fmt::vformat_to(std::back_inserter(buf), format_str, fmt::make_format_args(args...));
  buf.push_back(0);
  log((const char*)buf.data());
}

template <typename... Args>
void logWithLocation(const std::string& file, uint32_t line, const std::string func,
                     fmt::string_view format_str, Args&&... args)
{
  fmt::memory_buffer buf;
  fmt::vformat_to(std::back_inserter(buf), "{}:{} ({}) ", fmt::make_format_args(file, line, func));
  fmt::vformat_to(std::back_inserter(buf), format_str, fmt::make_format_args(args...));
  buf.push_back(0);
  log((const char*)buf.data());
}
}  // namespace os

#ifndef CLAP_WRAPPER_LOGLEVEL
#if NDEBUG
#define CLAP_WRAPPER_LOGLEVEL 0
#else
#define CLAP_WRAPPER_LOGLEVEL 2
#endif
#endif

#if (CLAP_WRAPPER_LOGLEVEL == 0)
#define LOGINFO(...) (void(0))
#define LOGDETAIL(...) (void(0))
#endif

#if (CLAP_WRAPPER_LOGLEVEL == 1)
#define LOGINFO os::logWithLocation(__FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOGDETAIL(...) (void(0))
#endif

#if (CLAP_WRAPPER_LOGLEVEL == 2)
#define LOGINFO(...) os::logWithLocation(__FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOGDETAIL(...) os::logWithLocation(__FILE__, __LINE__, __func__, __VA_ARGS__)
#endif
