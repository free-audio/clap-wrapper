

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <cstdint>
#include <cerrno>
#include <algorithm>
#include <string>

#include "x11_gui.h"
#include "linux_frontend.h"
#include <sys/epoll.h>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
namespace
{
int x11ErrorHandler(Display *d, XErrorEvent *e)
{
  // Xlib's default handler exits the process on any protocol error. A plugin
  // asking X11 for something it won't do shouldn't take the audio with it.
  char buf[512]{};
  XGetErrorText(d, e->error_code, buf, sizeof(buf) - 1);
  LOGINFO("[ERROR] X11 protocol error : {} (request {}.{})", buf, (int)e->request_code,
          (int)e->minor_code);
  fprintf(stderr, "[ERROR] X11 protocol error: %s (request %d.%d)\n", buf, (int)e->request_code,
          (int)e->minor_code);
  fflush(stderr);
  return 0;
}
}  // namespace

bool X11Gui::initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *sah)
{
  standaloneHost = sah;
  sah->x11Gui = this;
  sah->onRequestResize = [this](int w, int h) { return resetSizeTo(w, h); };

  // The epoll is how plugin timers and fds get dispatched, so it is set up
  // whether or not we end up with a display
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0)
  {
    LOGINFO("[ERROR] Unable to create epoll : {}", strerror(errno));
  }

  XInitThreads();
  XSetErrorHandler(x11ErrorHandler);

  display = XOpenDisplay(nullptr);
  if (!display)
  {
    const char *disp = getenv("DISPLAY");
    reportError("Unable to open a display",
                std::string("Could not connect to the X11 display '") +
                    (disp ? disp : "(DISPLAY not set)") +
                    "'. Continuing without a window; audio and MIDI still run.");
    return false;
  }

  return true;
}

bool X11Gui::keepRunning() const
{
  if (!runloopRunning) return false;
  if (quitRequested()) return false;
  if (standaloneHost && !standaloneHost->running) return false;
  return true;
}

void X11Gui::runloop()
{
  runloopRunning = true;

  if (epoll_fd < 0)
  {
    // Nothing to dispatch and nothing to draw, so just idle. This used to be a
    // 'while (true) sleep' which honoured nothing at all and could only be
    // ended by killing the process outright.
    while (keepRunning())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return;
  }

  XEvent e;
  struct epoll_event events[maxEpollEvents];
  while (keepRunning())
  {
    while (display && XPending(display))
    {
      XNextEvent(display, &e);
      switch (e.type)
      {
        case MapNotify:
        {
          if (guiCreated && plugin && plugin->_ext._gui)
          {
            clap_window win;
            win.api = CLAP_WINDOW_API_X11;
            win.x11 = window;
            auto ui = plugin->_ext._gui;
            if (!ui->set_parent(plugin->_plugin, &win))
            {
              reportError("Plugin Error",
                          "The plugin failed to embed its user interface. Please contact the "
                          "plugin developer.");
            }
            else
            {
              ui->show(plugin->_plugin);
            }
          }
        }
        break;
        case ConfigureNotify:
        {
          if (e.xconfigure.window == window)
          {
            handleConfigure(e.xconfigure.width, e.xconfigure.height);
          }
        }
        break;
        case ClientMessage:
        {
          // 3. Check if the message is the delete request
          if ((Atom)(e.xclient.data.l[0]) == wmDeleteMessage)
          {
            runloopRunning = false;
          }
          break;
        }
      }
    }
    if (!keepRunning()) break;

    // Poll for both X11 events and timer events
    auto num = epoll_wait(epoll_fd, events, maxEpollEvents, 50);

    if (num < 0)
    {
      if (errno == EINTR) continue;  // a signal; keepRunning() will sort it out
      LOGINFO("[ERROR] epoll_wait failed : {}", strerror(errno));
      break;
    }

    for (int i = 0; i < num; ++i)
    {
      auto fd = events[i].data.fd;
      auto tfd = fdToTimerId.find(fd);
      auto pfd = registeredFds.find(fd);

      if (tfd != fdToTimerId.end())
      {
        // The timerfd sits in a level-triggered epoll, so it has to be drained
        // here. Skipping the read leaves it permanently readable and epoll_wait
        // then returns immediately forever, spinning a core at 100%.
        uint64_t expirations{0};
        auto rd = ::read(fd, &expirations, sizeof(expirations));
        (void)rd;

        if (plugin && plugin->_ext._timer)
        {
          plugin->_ext._timer->on_timer(plugin->_plugin, tfd->second);
        }
      }
      if (pfd != registeredFds.end())
      {
        if (plugin && plugin->_ext._posixfd)
        {
          plugin->_ext._posixfd->on_fd(plugin->_plugin, fd, pfd->second);
        }
      }
    }
  }
}

void X11Gui::setPlugin(std::shared_ptr<Clap::Plugin> p)
{
  if (!p)
  {
    // The plugin failed to instantiate. main() reports that; don't compound it
    // with a null dereference here.
    LOGINFO("[ERROR] setPlugin with no plugin");
    return;
  }

  this->plugin = p;

  // Without a display we run on: the plugin still gets audio, MIDI, timers and
  // fd callbacks, it just has nowhere to draw
  if (!display) return;

  if (!plugin->_ext._gui)
  {
    LOGINFO("Plugin provides no GUI extension; running without a window");
    return;
  }

  auto ui = plugin->_ext._gui;
  auto pl = plugin->_plugin;

  if (!ui->is_api_supported(pl, CLAP_WINDOW_API_X11, false))
  {
    reportError("Plugin Error", "The plugin does not support an X11 GUI. Continuing without a window.");
    window = 0;
    return;
  }

  if (!ui->create(pl, CLAP_WINDOW_API_X11, false))
  {
    reportError("Plugin Error", "The plugin failed to create its user interface.");
    window = 0;
    return;
  }
  guiCreated = true;

  uint32_t w{0}, h{0};
  if (!ui->get_size(pl, &w, &h))
  {
    reportError("Plugin Error", "The plugin failed to report its window size.");
    destroyGui();
    return;
  }

  if (!isSaneSize(w, h))
  {
    reportError("Plugin Error", "The plugin reported an invalid window size (" + std::to_string(w) +
                                    " x " + std::to_string(h) + ").");
    destroyGui();
    return;
  }

  if (ui->can_resize(pl))
  {
    // adjust_size only means anything for a resizable GUI, and a zero back from
    // it would be a BadValue abort in XCreateSimpleWindow, so it only counts if
    // the answer is usable
    uint32_t aw{w}, ah{h};
    if (ui->adjust_size(pl, &aw, &ah) && isSaneSize(aw, ah))
    {
      w = aw;
      h = ah;
    }
  }

  int s = DefaultScreen(display);
  window = XCreateSimpleWindow(display, RootWindow(display, s), 10, 10, w, h, 1, BlackPixel(display, s),
                               WhitePixel(display, s));
  if (window == 0)
  {
    reportError("Unable to create a window", "X11 would not create the plugin window.");
    destroyGui();
    return;
  }

  XStoreName(display, window, plugin->_plugin->desc->name);
  // StructureNotify for map and resize, Exposure so a damaged window can be
  // asked to redraw. This used to pass InputOutput, which is a window class
  // rather than an event mask and happens to equal KeyPressMask.
  XSelectInput(display, window, StructureNotifyMask | ExposureMask);

  // Get window clsoed notifications
  wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wmDeleteMessage, 1);

  resetSizeTo(w, h);

  XMapWindow(display, window);

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = ConnectionNumber(display);
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ConnectionNumber(display), &event) == -1)
  {
    // Keep the epoll: plugin timers and fds still need it, and the runloop's
    // 50ms poll picks X events up regardless, just less promptly
    LOGINFO("[ERROR] Unable to register display epoll : {}", strerror(errno));
  }
}

bool X11Gui::isSaneSize(uint32_t w, uint32_t h)
{
  return w > 0 && h > 0 && w <= maxWindowDim && h <= maxWindowDim;
}

void X11Gui::destroyGui()
{
  if (guiCreated && plugin && plugin->_ext._gui)
  {
    plugin->_ext._gui->destroy(plugin->_plugin);
  }
  guiCreated = false;
  window = 0;
}
void X11Gui::shutdown()
{
  // only destroy a GUI we got as far as creating
  destroyGui();

  if (epoll_fd >= 0)
  {
    close(epoll_fd);
    epoll_fd = -1;
  }
  if (display && window > 0)
  {
    XDestroyWindow(display, window);
    XFlush(display);
  }
  if (display)
  {
    XCloseDisplay(display);
  }
  plugin.reset();
}

int X11Gui::nextTimerId{2112};

bool X11Gui::register_timer(uint32_t period_ms, clap_id *tid)
{
  if (epoll_fd < 0)
  {
    LOGINFO("[ERROR] register_timer with no epoll");
    return false;
  }

  // A zero period would spin the runloop, so 'as fast as you can' becomes 1ms.
  // An hour is well past any plausible redraw or housekeeping timer.
  auto period = std::clamp(period_ms, (uint32_t)1, (uint32_t)(60 * 60 * 1000));

  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (tfd < 0)
  {
    LOGINFO("[ERROR] timerfd_create failed : {}", strerror(errno));
    return false;
  }

  // tv_nsec has to stay below 1e9, so a period of a second or more belongs in
  // tv_sec. Stuffing it all into tv_nsec is EINVAL and the timer never fires.
  struct itimerspec ts;
  memset(&ts, 0, sizeof(ts));
  ts.it_interval.tv_sec = (time_t)(period / 1000);
  ts.it_interval.tv_nsec = (long)(period % 1000) * 1000000L;
  ts.it_value = ts.it_interval;

  if (timerfd_settime(tfd, 0, &ts, nullptr) < 0)
  {
    LOGINFO("[ERROR] timerfd_settime failed for a {}ms timer : {}", period, strerror(errno));
    close(tfd);
    return false;
  }

  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = tfd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &event) == -1)
  {
    LOGINFO("[ERROR] Unable to register timer epoll : {}", strerror(errno));
    close(tfd);
    return false;
  }

  auto id = nextTimerId;
  nextTimerId++;
  *tid = id;

  timerIdToFd[id] = tfd;
  fdToTimerId[tfd] = id;

  return true;
}
bool X11Gui::unregister_timer(clap_id tid)
{
  LOGINFO("Unregistering timer: {}", tid);
  if (epoll_fd < 0)
  {
    return false;
  }
  auto idF = timerIdToFd.find(tid);
  if (idF == timerIdToFd.end())
  {
    return false;
  }
  auto fd = idF->second;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1)
  {
    LOGINFO("epoll_ctl EPOLL_CTL_DEL failed to unregister timer");
    // Handle error
    return false;
  }
  close(fd);  // thereby stopping the timer
  timerIdToFd.erase(tid);
  fdToTimerId.erase(fd);
  return true;
}

uint32_t X11Gui::epollFlagsFor(clap_posix_fd_flags_t iflags)
{
  uint32_t flags{0};
  if (iflags & CLAP_POSIX_FD_READ) flags = flags | EPOLLIN;
  if (iflags & CLAP_POSIX_FD_WRITE) flags = flags | EPOLLOUT;
  if (iflags & CLAP_POSIX_FD_ERROR) flags = flags | EPOLLERR;
  return flags;
}

bool X11Gui::register_fd(int fd, clap_posix_fd_flags_t iflags)
{
  if (epoll_fd < 0) return false;

  epoll_event event{};
  event.events = epollFlagsFor(iflags);
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1)
  {
    LOGINFO("[ERROR] Unable to register plugin provided fd : {}", strerror(errno));
    return false;
  }
  registeredFds[fd] = iflags;
  return true;
}

bool X11Gui::modify_fd(int fd, clap_posix_fd_flags_t iflags)
{
  if (epoll_fd < 0) return false;

  auto pos = registeredFds.find(fd);
  if (pos == registeredFds.end())
  {
    LOGINFO("[ERROR] modify_fd on unregistered fd {}", fd);
    return false;
  }

  epoll_event event{};
  event.events = epollFlagsFor(iflags);
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == -1)
  {
    LOGINFO("[ERROR] epoll_ctl EPOLL_CTL_MOD failed for fd {} : {}", fd, strerror(errno));
    return false;
  }

  pos->second = iflags;
  return true;
}

bool X11Gui::unregister_fd(int fd)
{
  LOGINFO("Unregistering FD: {}", fd);
  if (epoll_fd < 0) return false;

  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1)
  {
    LOGINFO("epoll_ctl EPOLL_CTL_DEL failed");
    // Handle error
    return false;
  }

  registeredFds.erase(fd);
  return true;
}

bool X11Gui::resetSizeTo(int w, int h)
{
  if (!display || window == 0) return false;
  if (w <= 0 || h <= 0) return false;

  XResizeWindow(display, window, (unsigned int)w, (unsigned int)h);

  // we asked for this, so don't hand the resulting ConfigureNotify back to the
  // plugin as if the user had done it
  lastWidth = w;
  lastHeight = h;

  applySizeHints(w, h);

  return true;
}

void X11Gui::applySizeHints(int w, int h)
{
  bool hResize{false}, vResize{false}, keepAspect{false};
  int aspectW{0}, aspectH{0};

  if (guiCreated && plugin && plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    hResize = vResize = ui->can_resize(plugin->_plugin);

    if (hResize)
    {
      clap_gui_resize_hints_t rh{};
      if (ui->get_resize_hints && ui->get_resize_hints(plugin->_plugin, &rh))
      {
        hResize = rh.can_resize_horizontally;
        vResize = rh.can_resize_vertically;
        if (rh.preserve_aspect_ratio && rh.aspect_ratio_width > 0 && rh.aspect_ratio_height > 0)
        {
          keepAspect = true;
          aspectW = (int)rh.aspect_ratio_width;
          aspectH = (int)rh.aspect_ratio_height;
        }
      }
    }
  }

  XSizeHints *hints = XAllocSizeHints();
  if (!hints) return;

  // A fixed axis is pinned min == max. A resizable one gets a floor and no
  // practical ceiling: pinning both axes regardless of can_resize(), which is
  // what this used to do, makes user resize impossible.
  constexpr int minDim{64};
  hints->flags = PMinSize | PMaxSize;
  hints->min_width = hResize ? std::min(minDim, w) : w;
  hints->max_width = hResize ? (int)maxWindowDim : w;
  hints->min_height = vResize ? std::min(minDim, h) : h;
  hints->max_height = vResize ? (int)maxWindowDim : h;

  if (keepAspect)
  {
    hints->flags |= PAspect;
    hints->min_aspect.x = aspectW;
    hints->min_aspect.y = aspectH;
    hints->max_aspect.x = aspectW;
    hints->max_aspect.y = aspectH;
  }

  XSetWMNormalHints(display, window, hints);
  XFree(hints);
}

void X11Gui::handleConfigure(int w, int h)
{
  if (w <= 0 || h <= 0) return;
  if (w == lastWidth && h == lastHeight) return;  // our own resize coming back

  lastWidth = w;
  lastHeight = h;

  if (!guiCreated || !plugin || !plugin->_ext._gui) return;

  auto ui = plugin->_ext._gui;
  auto pl = plugin->_plugin;
  if (!ui->can_resize(pl)) return;  // a fixed size GUI has nothing to say here

  uint32_t aw{(uint32_t)w}, ah{(uint32_t)h};
  if (!ui->adjust_size(pl, &aw, &ah) || !isSaneSize(aw, ah))
  {
    aw = (uint32_t)w;
    ah = (uint32_t)h;
  }

  ui->set_size(pl, aw, ah);

  if ((int)aw != w || (int)ah != h)
  {
    // The plugin snapped to a size of its own, so make the window agree. The
    // ConfigureNotify that follows matches lastWidth/lastHeight, so this
    // settles rather than ping-ponging.
    lastWidth = (int)aw;
    lastHeight = (int)ah;
    XResizeWindow(display, window, aw, ah);
  }
}
};  // namespace freeaudio::clap_wrapper::standalone::linux_standalone