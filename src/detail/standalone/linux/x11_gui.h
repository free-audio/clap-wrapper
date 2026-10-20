#pragma once

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
  // below it the plugin's own window, where the plugin has a gui
  Window window{0};
  Window pluginWindow{0};
  Atom wmDeleteMessage{0};

  // the strip, drawn with the settings window's font and theme
  static constexpr int stripHeight{30};
  static constexpr int minimumWidth{420};
  UiFont font;
  Theme theme;
  GC gc{nullptr};
  Rect settingsButton;
  bool settingsButtonHot{false}, settingsButtonPressed{false};
  // Restore last session, a checkbox left of the settings button behind a rule, as the
  // other platforms have it in their menus; empty where there is no settings folder
  Rect restoreBox;
  bool restoreShownOn{false}, restorePressed{false};
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
