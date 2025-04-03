#pragma once

#include "fs.h"
#include <windows.h>

namespace os
{
fs::path getModulePath(HMODULE module);
}
