#pragma once

#include <clap_proxy.h>
#include <unordered_set>
#include "detail/standalone/standalone_host.h"

struct _GtkApplication;  // sigh their typedef screws up forward decls
namespace freeaudio::clap_wrapper::standalone::linux
{
struct GtkGui
{
  _GtkApplication *app{nullptr};
  std::shared_ptr<Clap::Plugin> plugin;

  void initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *);
  void setPlugin(std::shared_ptr<Clap::Plugin>);
  void runloop(int argc, char **argv);
  void shutdown();

  void setupPlugin(_GtkApplication *app);

  clap_id currTimer{8675309};
  std::mutex cbMutex;

  struct TimerCB
  {
    clap_id id;
    GtkGui *that;
  };
  std::unordered_set<std::unique_ptr<TimerCB>> timerCbs;
  std::unordered_set<clap_id> terminatedTimers;
  int runTimerFn(clap_id id);
  bool timersStarted{false};
  bool register_timer(uint32_t period_ms, clap_id *timer_id);
  bool unregister_timer(clap_id timer_id);

  struct FDCB
  {
    uint32_t ghandle;
    int fd;
    clap_posix_fd_flags_t flags;
    GtkGui *that;
  };
  std::unordered_set<std::unique_ptr<FDCB>> fdCbs;
  bool register_fd(int fd, clap_posix_fd_flags_t flags);
  // todo g_source_modify_unix_fd
  bool unregister_fd(int fd);
  int runFD(int fd, clap_posix_fd_flags_t flags);
};
}  // namespace freeaudio::clap_wrapper::standalone::linux