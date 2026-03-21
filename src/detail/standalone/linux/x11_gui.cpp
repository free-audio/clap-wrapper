

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <poll.h>
#include <string.h>
#include <cstdint>

#include "x11_gui.h"
#include <sys/epoll.h>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
void X11Gui::initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *sah)
{
  display = XOpenDisplay(nullptr);
  sah->x11Gui = this;
  sah->onRequestResize = [this](int w, int h) { return resetSizeTo(w, h); };
  epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0)
  {
    LOGINFO("Unable to create epoll");
  }
}

void X11Gui::runloop()
{
  if (!display || window == 0 || epoll_fd < 0)
  {
    // no gui. Only way to kill us is with a signal.
    while (true)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    return;
  }
  XEvent e;
  struct epoll_event events[maxEpollEvents];
  bool running{true};
  while (running)
  {
    while (XPending(display))
    {
      XNextEvent(display, &e);
      switch (e.type)
      {
        case MapNotify:
        {
          clap_window win;
          win.api = CLAP_WINDOW_API_X11;
          win.x11 = window;
          auto ui = plugin->_ext._gui;
          ui->set_parent(plugin->_plugin, &win);
          ui->show(plugin->_plugin);
        }
        break;
        case ClientMessage:
        {
          // 3. Check if the message is the delete request
          if ((Atom)(e.xclient.data.l[0]) == wmDeleteMessage)
          {
            running = false;
          }
          break;
        }
      }
    }
    // Poll for both X11 events and timer events
    int num{0};
    if (running) num = epoll_wait(epoll_fd, events, maxEpollEvents, 50);

    if (num > 0)
    {
      for (int i = 0; i < num; ++i)
      {
        auto fd = events[i].data.fd;
        auto tfd = fdToTimerId.find(fd);
        auto pfd = registeredFds.find(fd);

        if (tfd != fdToTimerId.end())
        {
          if (plugin->_ext._timer)
          {
            plugin->_ext._timer->on_timer(plugin->_plugin, tfd->second);
          }
        }
        if (pfd != registeredFds.end())
        {
          if (plugin->_ext._posixfd)
          {
            plugin->_ext._posixfd->on_fd(plugin->_plugin, fd, pfd->second);
          }
        }
      }
    }
  }
}

void X11Gui::setPlugin(std::shared_ptr<Clap::Plugin> p)
{
  this->plugin = p;
  if (display && plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto p = plugin->_plugin;
    if (!ui->is_api_supported(p, CLAP_WINDOW_API_X11, false))
    {
      LOGINFO("[ERROR] CLAP does not support X11");
      window = 0;
      return;
    }

    ui->create(p, CLAP_WINDOW_API_X11, false);

    uint32_t w, h;
    ui->get_size(p, &w, &h);
    ui->adjust_size(p, &w, &h);

    int s = DefaultScreen(display);
    window = XCreateSimpleWindow(display, RootWindow(display, s), 10, 10, w, h, 1,
                                 BlackPixel(display, s), WhitePixel(display, s));
    XStoreName(display, window, plugin->_plugin->desc->name);
    XSelectInput(display, window, InputOutput | StructureNotifyMask);

    // Get window clsoed notifications
    wmDeleteMessage = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wmDeleteMessage, 1);

    resetSizeTo(w, h);

    XMapWindow(display, window);

    epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = ConnectionNumber(display);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ConnectionNumber(display), &event) == -1)
    {
      LOGINFO("Unable to register display epoll");
      close(epoll_fd);
      epoll_fd = -1;
      return;
    }
  }
}
void X11Gui::shutdown()
{
  if (epoll_fd >= 0)
  {
    close(epoll_fd);
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
}

int X11Gui::nextTimerId{2112};

bool X11Gui::register_timer(int period_ms, clap_id *tid)
{
  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  struct itimerspec ts;
  memset(&ts, 0, sizeof(ts));
  ts.it_interval.tv_nsec = period_ms * 1e6;  // Repeat every 1s
  ts.it_value.tv_nsec = period_ms * 1e6;     // First expiry in 1s
  timerfd_settime(tfd, 0, &ts, NULL);

  epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = tfd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &event) == -1)
  {
    LOGINFO("Unable to register timer epoll");
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

bool X11Gui::register_fd(int fd, clap_posix_fd_flags_t iflags)
{
  int flags{0};
  if (iflags & CLAP_POSIX_FD_READ) flags = flags | EPOLLIN;
  if (iflags & CLAP_POSIX_FD_WRITE) flags = flags | EPOLLOUT;
  if (iflags & CLAP_POSIX_FD_ERROR) flags = flags | EPOLLERR;

  epoll_event event;
  event.events = flags;
  event.data.fd = fd;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) == -1)
  {
    LOGINFO("Unable to register plugin provided fd");
    return false;
  }
  registeredFds[fd] = iflags;
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
  XResizeWindow(display, window, w, h);

  XSizeHints *hints = XAllocSizeHints();
  hints->flags = PMinSize | PMaxSize;
  hints->min_width = w;
  hints->max_width = w;
  hints->min_height = h;
  hints->max_height = h;

  // Apply hints to the window
  XSetWMNormalHints(display, window, hints);
  XFree(hints);

  return true;
}
};  // namespace freeaudio::clap_wrapper::standalone::linux_standalone