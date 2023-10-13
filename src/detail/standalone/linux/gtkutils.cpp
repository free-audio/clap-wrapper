#if CLAP_WRAPPER_HAS_GTK3

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib.h>
#include <glib-unix.h>

#include "gtkutils.h"
#include "detail/standalone/standalone_details.h"

#include <cassert>

namespace freeaudio::clap_wrapper::standalone::linux
{

static void activate(GtkApplication *app, gpointer user_data)
{
  auto g = (GtkGui *)user_data;
  g->setupPlugin(app);
}

void GtkGui::setupPlugin(_GtkApplication *app)
{
  GtkWidget *window;

  auto pluginProxy = plugin->getProxy();
  if (pluginProxy->canUseGui())
  {
    if (!pluginProxy->guiIsApiSupported(CLAP_WINDOW_API_X11, false)) LOG << "NO X11 " << std::endl;

    pluginProxy->guiCreate(CLAP_WINDOW_API_X11, false);
    pluginProxy->guiSetScale(1);

    uint32_t w, h;
    pluginProxy->guiGetSize(w, &h);

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Standalone Window");
    gtk_window_set_default_size(GTK_WINDOW(window), w, h);
    gtk_widget_show_all(window);

    clap_window win;
    win.api = CLAP_WINDOW_API_X11;
    auto gw = gtk_widget_get_window(GTK_WIDGET(window));
    win.x11 = GDK_WINDOW_XID(gw);
    pluginProxy->guiSetParent(win);
    pluginProxy->guiShow();
  }
}

void GtkGui::initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *sah)
{
  sah->gtkGui = this;
  app = gtk_application_new("org.gtk.example", G_APPLICATION_FLAGS_NONE);
  g_signal_connect(app, "activate", G_CALLBACK(activate), this);
}

void GtkGui::setPlugin(std::shared_ptr<Clap::Plugin> p)
{
  plugin = p;
}

void GtkGui::runloop(int argc, char **argv)
{
  g_application_run(G_APPLICATION(app), argc, argv);
}

void GtkGui::shutdown()
{
  g_object_unref(app);
}

int gtimercb(void *ud)
{
  auto tcb = (GtkGui::TimerCB *)ud;
  return tcb->that->runTimerFn(tcb->id);
}

bool GtkGui::register_timer(uint32_t period_ms, clap_id *timer_id)
{
  std::lock_guard<std::mutex> g{cbMutex};

  auto nid = currTimer++;
  *timer_id = nid;

  auto nto = std::make_unique<TimerCB>();
  nto->id = nid;
  nto->that = this;

  g_timeout_add(period_ms, gtimercb, nto.get());

  timerCbs.insert(std::move(nto));
  return true;
}

bool GtkGui::unregister_timer(clap_id timer_id)
{
  std::lock_guard<std::mutex> g{cbMutex};
  terminatedTimers.insert(timer_id);
  return true;
}

int GtkGui::runTimerFn(clap_id id)
{
  std::lock_guard<std::mutex> g{cbMutex};
  if (terminatedTimers.find(id) != terminatedTimers.end()) return false;

  if (plugin->getProxy()->canUseTimer()) plugin->getProxy()->timerSupportOnTimer(id);
  return true;
}

int gfdcb(int fd, GIOCondition cond, void *ud)
{
  auto that = (GtkGui::FDCB *)ud;

  assert(fd == that->fd);

  return that->that->runFD(fd, that->flags);
}

bool GtkGui::register_fd(int fd, clap_posix_fd_flags_t flags)
{
  std::lock_guard<std::mutex> g{cbMutex};

  auto nto = std::make_unique<FDCB>();
  nto->fd = fd;
  nto->that = this;
  nto->flags = flags;

  int cd{0};
  if (flags & CLAP_POSIX_FD_READ) cd = cd | G_IO_IN | G_IO_PRI;
  if (flags & CLAP_POSIX_FD_WRITE) cd = cd | G_IO_OUT;
  if (flags & CLAP_POSIX_FD_ERROR) cd = cd | G_IO_ERR;

  nto->ghandle = g_unix_fd_add(fd, (GIOCondition)cd, gfdcb, nto.get());

  fdCbs.insert(std::move(nto));

  return true;
}
bool GtkGui::unregister_fd(int fd)
{
  std::lock_guard<std::mutex> g{cbMutex};
  for (const auto &f : fdCbs)
  {
    if (f->fd == fd && f->ghandle)
    {
      g_source_remove(f->ghandle);
      f->ghandle = 0;
    }
  }
  return true;
}

int GtkGui::runFD(int fd, clap_posix_fd_flags_t flags)
{
  if (plugin->getProxy()->canUsePosixFdSupport())
  {
    plugin->getProxy()->posixFdSupportOnFd(fd, flags);
  }
  return true;
}

}  // namespace freeaudio::clap_wrapper::standalone::linux

#endif
