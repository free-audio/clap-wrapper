#pragma once

#define FMT_HEADER_ONLY
#include "fmt/format.h"
#include "fmt/ranges.h"

#ifndef CLAP_WRAPPER_LOGLEVEL
#if NDEBUG
#define CLAP_WRAPPER_LOGLEVEL 0
#else
#define CLAP_WRAPPER_LOGLEVEL 2
#endif
#endif

#if CLAP_WRAPPER_LOGLEVEL == 0
#define LOGINFO(...) (void(0))
#define LOGDETAIL(...) (void(0))

#else

#if WIN
#include <windows.h>
#endif
#include <stdio.h>

namespace os
{

inline void log(const char* text)
{
#if WIN
  OutputDebugStringA(text);
  OutputDebugStringA("\n");
#else
  fprintf(stdout, "%s\n", text);
  fflush(stdout);
#endif
}

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
#endif
