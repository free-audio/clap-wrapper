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
  // false if there is no usable display, or none was wanted; audio, MIDI and
  // plugin timers all still run, we just never get a window
  bool initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *, bool wantWindow = true);
  void setPlugin(std::shared_ptr<Clap::Plugin>);
  void runloop();
  void shutdown();

  // false once the window is closed, the host stops running, or we are signalled
  bool keepRunning() const;

  bool register_timer(uint32_t period_ms, clap_id *tid);
  bool unregister_timer(clap_id tid);

  bool register_fd(int fd, clap_posix_fd_flags_t flags);
  bool modify_fd(int fd, clap_posix_fd_flags_t flags);
  bool unregister_fd(int fd);

  static uint32_t epollFlagsFor(clap_posix_fd_flags_t flags);

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

  // WM size hints from the plugin's resize hints, rather than pinning
  // min == max == current for everything
  void applySizeHints(int w, int h);
  // a user (or WM) resize of our window, handed on to the plugin
  void handleConfigure(int w, int h);

  static constexpr uint32_t maxWindowDim{16384};
  static bool isSaneSize(uint32_t w, uint32_t h);
  void destroyGui();

  // last size we know the window to have, so our own resizes don't echo back
  // into the plugin
  int lastWidth{-1}, lastHeight{-1};

  std::map<int, clap_id> fdToTimerId;
  std::map<clap_id, int> timerIdToFd;

  std::map<int, int> registeredFds;

  static int nextTimerId;
};
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
