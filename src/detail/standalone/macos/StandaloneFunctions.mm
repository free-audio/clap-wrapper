

#include <Foundation/Foundation.h>
#include "detail/standalone/standalone_host.h"

namespace freeaudio::clap_wrapper::standalone
{
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
