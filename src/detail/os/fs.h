#pragma once

#if MAC && !MACOS_USE_STD_FILESYSTEM
#include "ghc/filesystem.hpp"
namespace fs = ghc::filesystem;
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif
