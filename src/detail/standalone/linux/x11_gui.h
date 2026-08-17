#pragma once

#include <clap_proxy.h>
#include <unordered_set>
#include "detail/standalone/standalone_host.h"
#include <poll.h>
#include <map>
#include <set>

#include <X11/Xlib.h>
namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
struct X11Gui
{
  // false if there is no usable display; audio and timers still run, we just
  // never get a window
  bool initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *);
  void setPlugin(std::shared_ptr<Clap::Plugin>);
  void runloop();
  void shutdown();

  // false once the window is closed, the host stops running, or we are signalled
  bool keepRunning() const;

  bool register_timer(uint32_t period_ms, clap_id *tid);
  bool unregister_timer(clap_id tid);

  bool register_fd(int fd, clap_posix_fd_flags_t flags);
  bool unregister_fd(int fd);

  freeaudio::clap_wrapper::standalone::StandaloneHost *standaloneHost{nullptr};

  Display *display{nullptr};
  Window window{0};
  Atom wmDeleteMessage{0};
  bool runloopRunning{false};
  // so shutdown() only destroys a GUI we actually created
  bool guiCreated{false};

  int epoll_fd{-1};
  static constexpr size_t maxEpollEvents{256};

  std::shared_ptr<Clap::Plugin> plugin{nullptr};

  bool resetSizeTo(int w, int h);

  static bool isSaneSize(uint32_t w, uint32_t h);
  void destroyGui();

  std::map<int, clap_id> fdToTimerId;
  std::map<clap_id, int> timerIdToFd;

  std::map<int, int> registeredFds;

  static int nextTimerId;
};
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
