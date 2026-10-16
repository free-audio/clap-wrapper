

#include <Foundation/Foundation.h>
#include "detail/standalone/standalone_host.h"

namespace freeaudio::clap_wrapper::standalone
{
uint32_t supportedScreenAppearances()
{
  uint32_t res{(1u << CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO) |
               (1u << CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT)};
  // dark aqua came with macos 10.14; before it, light is all there is
  if (@available(macOS 10.14, *)) res |= 1u << CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK;
  return res;
}

std::optional<fs::path> getStandaloneSettingsPath()
{
  auto *fileManager = [NSFileManager defaultManager];
  auto *resultURLs = [fileManager URLsForDirectory:NSApplicationSupportDirectory
                                         inDomains:NSUserDomainMask];
  if (resultURLs)
  {
    auto *u = [resultURLs objectAtIndex:0];
    return fs::path{[u fileSystemRepresentation]} / "clap-wrapper-standalone";
  }
  return std::nullopt;
}
}  // namespace freeaudio::clap_wrapper::standalone
