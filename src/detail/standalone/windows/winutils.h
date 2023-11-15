#pragma once

#include <clap_proxy.h>
#include "detail/standalone/standalone_host.h"

namespace freeaudio::clap_wrapper::standalone::windows
{
struct Win32Gui
{
  std::shared_ptr<Clap::Plugin> plugin;
};
}  // namespace freeaudio::clap_wrapper::standalone::windows
