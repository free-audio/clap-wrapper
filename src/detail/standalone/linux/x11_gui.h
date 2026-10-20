#pragma once

#include <algorithm>
#include <clap_proxy.h>
#include <unordered_set>
#include "detail/standalone/standalone_host.h"
#include <poll.h>
#include <map>
#include <set>
#include <chrono>
#include <memory>
#include <optional>

#include "x11_ui.h"
#include "x11_settings.h"

#include <X11/Xlib.h>
namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
struct X11Gui
{
  void initialize(freeaudio::clap_wrapper::standalone::StandaloneHost *);
  void setPlugin(std::shared_ptr<Clap::Plugin>);
  void runloop();
  void shutdown();

  // the audio settings into standalone-settings.conf: on a first run, and on quitting, as
  // the Windows frontend does besides its settings window's OK
  void saveSettings();

  bool register_timer(int period_ms, clap_id *tid);
  bool unregister_timer(clap_id tid);

  bool register_fd(int fd, clap_posix_fd_flags_t flags);
  bool unregister_fd(int fd);

  Display *display{nullptr};
  // the standalone's window: a strip with the settings button and the audio's state, and
  // below it the plugin's own window, where the plugin has a gui. the strip is left out
  // where there is a gui unless the build turns CLAP_WRAPPER_STANDALONE_X11_HIDE_STRIP off
  Window window{0};
  Window pluginWindow{0};
  Atom wmDeleteMessage{0};

  // the strip, drawn with the settings window's font and theme
  static constexpr int stripHeight{30};
  static constexpr int minimumWidth{420};
#if CLAP_WRAPPER_STANDALONE_X11_HIDE_STRIP
  static constexpr bool hideStrip{true};
#else
  static constexpr bool hideStrip{false};
#endif
  // whether the window has the strip: unless the build hides it, and always for a plugin
  // without a gui, whose window would otherwise be empty, with no way to the settings
  bool showStrip{true};
  int stripShown() const
  {
    return showStrip ? stripHeight : 0;
  }
  int widthFor(int width) const
  {
    return showStrip ? std::max(width, minimumWidth) : std::max(width, 1);
  }
  UiFont font;
  Theme theme;
  GC gc{nullptr};
  Rect settingsButton;
  bool settingsButtonHot{false}, settingsButtonPressed{false};
  std::string status;
  bool statusIsError{false};
  int pluginWidth{0}, pluginHeight{0};
  void layoutWindow();
  void drawStrip();
  void updateStatus();
  void handleStrip(const XEvent &event);

  // the Audio/MIDI settings window, while it is open; the result of one that closed,
  // reported to the plugin on the loop's next turn
  std::unique_ptr<SettingsWindow> settings;
  std::optional<uint32_t> settingsClosed;
  void openSettings();
  std::string lastError;

  int epoll_fd{-1};
  static constexpr size_t maxEpollEvents{256};

  std::shared_ptr<Clap::Plugin> plugin{nullptr};

  // the host whose audio events the loop takes, as the macOS and Windows timers do
  freeaudio::clap_wrapper::standalone::StandaloneHost *host{nullptr};
  std::chrono::steady_clock::time_point nextAudioPoll{};
  void pollAudioEvents();

  bool resetSizeTo(int w, int h);

  std::map<int, clap_id> fdToTimerId;
  std::map<clap_id, int> timerIdToFd;

  std::map<int, int> registeredFds;

  static int nextTimerId;
};
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
