#if CLAP_WRAPPER_HAS_GTK3

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <glib.h>
#include <glib-unix.h>

#include "gtkutils.h"
#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"
#include "detail/standalone/entry.h"

#include <cassert>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{

static void activate(GtkApplication *app, gpointer user_data)
{
  GdkDisplay *display = gdk_display_get_default();

  if (!GDK_IS_X11_DISPLAY(display))
  {
    std::cout << "clap-wrapper standalone requires GDK X11 backend" << std::endl;
    std::terminate();
  }

  auto g = (GtkGui *)user_data;
  g->setupPlugin(app);
}

static gboolean onResize(GtkWidget *wid, GdkEventConfigure *event, gpointer user_data)
{
  auto g = (GtkGui *)user_data;
  return g->resizePlugin(wid, event->width, event->height);
}

bool GtkGui::resizePlugin(GtkWidget *wid, uint32_t w, uint32_t h)
{
  if (plugin->_ext._gui)
  {
    auto gui = plugin->_ext._gui;
    auto p = plugin->_plugin;

    if (!gui->can_resize(p))
    {
      gui->get_size(p, &w, &h);
      gtk_window_resize(GTK_WINDOW(wid), w, h);
      return TRUE;
    }

#if 1
    gui->set_size(p, w, h);
#else
    // For some reason, this freaks out with drags in surge on gtk on linux.
    auto adj = false;
    auto aw = w, ah = h;
    gui->adjust_size(p, &aw, &ah);
    gui->set_size(p, aw, ah);

    if (aw != w || ah != h) adj = true;

    w = aw;
    h = ah;

    gtk_window_resize(GTK_WINDOW(wid), w, h);

    return adj;
#endif
  }
  return FALSE;
}

void GtkGui::setupPlugin(_GtkApplication *app)
{
  GtkWidget *window;

  if (plugin->_ext._gui)
  {
    auto ui = plugin->_ext._gui;
    auto p = plugin->_plugin;
    if (!ui->is_api_supported(p, CLAP_WINDOW_API_X11, false)) LOG << "NO X11 " << std::endl;

    ui->create(p, CLAP_WINDOW_API_X11, false);
    ui->set_scale(p, 1);

    uint32_t w, h;
    ui->get_size(p, &w, &h);
    ui->adjust_size(p, &w, &h);

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), p->desc->name);
    gtk_window_set_default_size(GTK_WINDOW(window), w, h);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    // Create the 'inner window'
    GtkWidget *frame = gtk_frame_new("Inner 'Window'");
    gtk_widget_set_size_request(frame, w, h);
    gtk_box_pack_start(GTK_BOX(vbox), frame, TRUE, TRUE, 0);

    g_signal_connect(window, "configure-event", G_CALLBACK(onResize), this);

    gtk_widget_show_all(window);

    clap_window win;
    win.api = CLAP_WINDOW_API_X11;
    auto gw = gtk_widget_get_window(GTK_WIDGET(frame));
    win.x11 = GDK_WINDOW_XID(gw);
    ui->set_parent(p, &win);
    ui->show(p);
  }
}

void GtkGui::initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *sah)
{
  sah->gtkGui = this;
  app = gtk_application_new("org.gtk.example", G_APPLICATION_DEFAULT_FLAGS);
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

  if (plugin->_ext._timer) plugin->_ext._timer->on_timer(plugin->_plugin, id);
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
  if (plugin->_ext._posixfd)
  {
    plugin->_ext._posixfd->on_fd(plugin->_plugin, fd, flags);
  }
  return true;
}

bool GtkGui::parseCommandLine(int argc, char **argv)
{
  auto sah = freeaudio::clap_wrapper::standalone::getStandaloneHost();

  auto [i, o, s] = sah->getDefaultAudioInOutSampleRate();

  bool list_devices{false};
  int sampleRate{s};
  unsigned int inId{i}, outId{o};

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored \
    "-Wmissing-field-initializers"  // other peoples errors are outside my scope
#endif
  const GOptionEntry entries[] = {
      {"list-devices", 'l', 0, G_OPTION_ARG_NONE, &list_devices, "List Input Output and MIDI Devices",
       nullptr},
      {"sample-rate", 's', 0, G_OPTION_ARG_INT, &sampleRate, "Sample Rate", nullptr},
      {"input-device", 'i', 0, G_OPTION_ARG_INT, &inId, "Input Device (0 for no input)", nullptr},
      {"output-device", 'o', 0, G_OPTION_ARG_INT, &outId, "Output Device (0 for no input)", nullptr},
      {NULL}};
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

  GOptionContext *context;
  GError *error = nullptr;

  context = g_option_context_new("Clap Wrapper Standalone");
  g_option_context_add_main_entries(context, entries, NULL);

  g_option_context_add_group(context, gtk_get_option_group(TRUE));
  if (!g_option_context_parse(context, &argc, &argv, &error))
  {
    g_print("Failed to parse options: %s\n", error->message);
    g_error_free(error);
    return false;
  }

  if (list_devices)
  {
    std::cout << "\n\nAvailable Audio Interfaces:\n\nOutput:\n";
    auto outD = sah->getOutputAudioDevices();
    for (auto &d : outD)
    {
      std::cout << "  - " << d.name << " (id=" << d.ID << " channels=" << d.outputChannels << ")"
                << std::endl;
    }

    std::cout << "\nInput:\n";
    auto inD = sah->getInputAudioDevices();
    for (auto &d : inD)
    {
      std::cout << "  - " << d.name << " (id=" << d.ID << " channels=" << d.outputChannels << ")"
                << std::endl;
    }
    return false;
  }

  LOG << "Post Argument Parse: inId=" << inId << " outId=" << outId << " sampleRate=" << sampleRate
      << std::endl;
  sah->setStartupAudio(inId, outId, sampleRate);

  return true;
}

}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone

#endif
