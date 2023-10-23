#pragma once

#include <clap_proxy.h>
#include "detail/standalone/standalone_host.h"

namespace freeaudio::clap_wrapper::standalone::windows
{
struct Win32Gui
{
  std::shared_ptr<Clap::Plugin> plugin;

  void initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *);
  void setPlugin(std::shared_ptr<Clap::Plugin>);
  void run();
};
}  // namespace freeaudio::clap_wrapper::standalone::windows
