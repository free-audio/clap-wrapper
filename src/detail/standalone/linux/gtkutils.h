#pragma once

#include <clap_proxy.h>

struct _GtkApplication;  // sigh their typedef screws up forward decls
namespace Clap::Standalone::Linux
{
struct GtkGui
{
  _GtkApplication *app{nullptr};
  std::shared_ptr<Clap::Plugin> plugin;

  void initialize();
  void setPlugin(std::shared_ptr<Clap::Plugin>);
  void runloop(int argc, char **argv);
  void shutdown();

  void setupPlugin(_GtkApplication *app);
};
}  // namespace Clap::Standalone::Linux