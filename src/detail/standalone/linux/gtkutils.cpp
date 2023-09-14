#if CLAP_WRAPPER_HAS_GTK3

#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include "gtkutils.h"
#include "detail/standalone/standalone_details.h"

namespace Clap::Standalone::Linux
{

static void activate(GtkApplication *app, gpointer user_data)
{
  auto g = (GtkGui *)user_data;
  g->setupPlugin(app);
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

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Standalone Window");
    gtk_window_set_default_size(GTK_WINDOW(window), w, h);
    gtk_widget_show_all(window);

    clap_window win;
    win.api = CLAP_WINDOW_API_X11;
    auto gw = gtk_widget_get_window(GTK_WIDGET(window));
    win.x11 = GDK_WINDOW_XID(gw);
    ui->set_parent(p, &win);
    ui->show(p);
  }
}

void GtkGui::initialize()
{
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

}  // namespace Clap::Standalone::Linux

#endif
