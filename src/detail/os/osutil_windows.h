#pragma once

#include "fs.h"
#include <windows.h>

namespace os
{

static fs::path getModulePath(HMODULE mod)
{
  fs::path modulePath{};
  DWORD bufferSize = 150;  // Start off with a reasonably large size
  std::wstring buffer(bufferSize, L'\0');

  DWORD length = GetModuleFileNameW(mod, buffer.data(), bufferSize);

  constexpr size_t maxExtendedPath = 0x7FFF - 24;  // From Windows Implementation Library

  while (length == bufferSize && GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
         bufferSize < maxExtendedPath)
  {
    bufferSize *= 2;
    buffer.resize(bufferSize);
    length = GetModuleFileNameW(mod, buffer.data(), bufferSize);
  }
  buffer.resize(length);
  modulePath = std::move(buffer);
  return modulePath;
}

}  // namespace os
