#pragma once

#include <Windows.h>
#include <ShlObj.h>

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

#define FMT_HEADER_ONLY 1
#include <fmt/format.h>
#include <fmt/xchar.h>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_host.h"

namespace freeaudio::clap_wrapper::standalone::windows_standalone
{
std::vector<std::string> getArgs();
::HMODULE getInstance();
::HFONT getFontFromSystem(int name = DEFAULT_GUI_FONT);
::HFONT getScaledFontFromSystem(double scale);
::HBRUSH getBrushFromSystem(int name = BLACK_BRUSH);
::HCURSOR getCursorFromSystem(::LPCWSTR name = IDC_ARROW);
::HICON getIconFromSystem(::LPCWSTR name = IDI_APPLICATION);
::HICON getIconFromResource();
::HMENU getSystemMenu(::HWND hwnd);

std::wstring toUTF16(std::string_view utf8);
std::string toUTF8(std::wstring_view utf16);

std::string formatMessage(::HRESULT errorCode);
std::string getLastError();

void log(const std::string &message);
void log(const std::wstring &message);

template <typename... Args>
void log(const fmt::format_string<Args...> fmt, Args &&...args)
{
  ::OutputDebugStringW(toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str());
  ::OutputDebugStringW(L"\n");
}

template <typename... Args>
void log(const fmt::wformat_string<Args...> fmt, Args &&...args)
{
  ::OutputDebugStringW(fmt::vformat(fmt.get(), fmt::make_wformat_args(args...)).c_str());
  ::OutputDebugStringW(L"\n");
}

int run();
void abort(int exitCode = EXIT_FAILURE);
void quit(int exitCode = EXIT_SUCCESS);

struct Message
{
  ::HWND hwnd;
  ::UINT msg;
  ::WPARAM wparam;
  ::LPARAM lparam;
};

struct MessageHandler
{
  using MessageCallback = std::function<::LRESULT(Message)>;

  bool on(::UINT msg, MessageCallback callback);
  bool contains(::UINT msg);
  ::LRESULT invoke(Message message);

  template <typename W, typename L>
  ::LRESULT send(::HWND hwnd, ::UINT msg, W wparam, L lparam)
  {
    return ::SendMessageW(hwnd, msg, (::WPARAM)wparam, (::LPARAM)lparam);
  }

  void box(const std::string &message);
  void box(const std::wstring &message);

  void error(const std::string &errorMessage);
  void error(const std::wstring &errorMessage);

  template <typename... Args>
  void box(const fmt::format_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(nullptr, toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str(),
                  nullptr, MB_OK | MB_ICONASTERISK);
  }

  template <typename... Args>
  void box(const fmt::wformat_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(nullptr, fmt::vformat(fmt.get(), fmt::make_wformat_args(args...)).c_str(), nullptr,
                  MB_OK | MB_ICONASTERISK);
  }

  template <typename... Args>
  void error(const fmt::format_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(nullptr, toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str(),
                  nullptr, MB_OK | MB_ICONHAND);
  }

  template <typename... Args>
  void error(const fmt::wformat_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(nullptr, fmt::vformat(fmt.get(), fmt::make_wformat_args(args...)).c_str(), nullptr,
                  MB_OK | MB_ICONHAND);
  }

 private:
  std::unordered_map<::UINT, MessageCallback> map;
};

struct Position
{
  int32_t x{0};
  int32_t y{0};
  uint32_t width{0};
  uint32_t height{0};
};

// True when the rectangle intersects a connected display.
bool isRectOnAnyMonitor(const Position &position);

struct Window
{
  void create(const std::string &title);

  static ::LRESULT CALLBACK procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam);

  void activate();
  void show();
  void hide();
  bool isVisible();

  void adjustSize(uint32_t width, uint32_t height);
  void setPosition(Position position);
  void setStyle(::LONG_PTR style);

  void toggleCentered(bool centered);
  void toggleTopmost(bool topmost);

  void repaint();

  bool startTimer(::UINT_PTR timerId, ::UINT intervalMs);
  bool stopTimer(::UINT_PTR timerId);

  Position window;
  Position client;
  Position monitor;
  ::WINDOWPLACEMENT placement;
  uint32_t dpi;
  double scale;

  Position restore;
  Position suggested;

  MessageHandler message;
  wil::unique_hwnd hwnd;
};

struct Control
{
  void setPosition(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void refreshFont(double scale);

  Position position;

  wil::unique_hfont font;

  MessageHandler message;
  wil::unique_hwnd hwnd;
};

struct ComboBox final : public Control
{
  void create(const std::string &name, uintptr_t id, ::HWND parentHwnd);
  void reset();
  void add(const std::string &string);

  bool set(int index);
  bool set(const std::string &searchString);
  ::LRESULT get();

  // get() returns CB_ERR (-1) when nothing is selected, which as a vector index is
  // an out-of-bounds read. Callers index containers with this instead, and it is
  // empty unless the selection is both present and in range.
  std::optional<size_t> selection(size_t containerSize);

  ::LRESULT getItemHeight();

  static ::LRESULT CALLBACK procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam,
                                      ::UINT_PTR id, ::DWORD_PTR data);
};

struct ListBox final : public Control
{
  void create(const std::string &name, uintptr_t id, ::HWND parentHwnd);
  void reset();
  void add(const std::string &string);

  bool set(int index);
  bool set(const std::string &searchString);
  void clearSelection();
  ::LRESULT get();
  ::LRESULT getItems(std::vector<int> &buffer);
  ::LRESULT getItemsCount();
  ::LRESULT getItemHeight();

  static ::LRESULT CALLBACK procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam,
                                      ::UINT_PTR id, ::DWORD_PTR data);
};

struct SystemMenu
{
  void add(std::wstring &name, ::UINT id);
  void addToggle(std::wstring &name, ::UINT id, bool checked = false);
  void addSeparator();
  void populate(::HWND hwnd);

  std::vector<::MENUITEMINFOW> item;
};

// Posted to the plugin window to show a queued audio error from the main loop
// rather than from inside whatever handler or backend thread produced it.
constexpr ::UINT WM_SHOW_AUDIO_ERROR{WM_APP + 1};

struct Plugin final : public Window
{
  struct Menu final : public SystemMenu
  {
    enum Identifier
    {
      AudioMidiSettings,
      MuteInput,
      SaveState,
      LoadState,
      ResetState
    };

    std::wstring audioMidiSettings{L"Audio/MIDI Settings"};
    std::wstring muteInput{L"Mute input"};
    std::wstring saveState{L"Save state..."};
    std::wstring loadState{L"Load state..."};
    std::wstring resetState{L"Reset state"};
  };

  struct Settings final : public Window
  {
    enum Identifier
    {
      AudioApi,
      AudioOutput,
      AudioInput,
      AudioSamplerate,
      AudioBuffersize,
      MidiInputs
    };

    ComboBox api;
    ComboBox output;
    ComboBox input;
    ComboBox sampleRate;
    ComboBox bufferSize;
    ListBox midiIn;
  };

  struct ClapPlugin
  {
    std::shared_ptr<Clap::Plugin> clap;
    const clap_plugin_t *plugin;
    const clap_plugin_gui_t *gui;
    const clap_plugin_state_t *state;
  };

  explicit Plugin(std::shared_ptr<Clap::Plugin> clapPlugin, int nCmdShow = SW_SHOWDEFAULT);

  std::optional<clap_gui_resize_hints> getResizeHints();
  void refreshLayout();
  void refreshApis();
  void refreshOutputs();
  void refreshInputs();
  void refreshSampleRates();
  void refreshBufferSizes();
  void refreshMIDIInputs();

  bool saveSettings();
  bool loadSettings();

  void startMIDI();

  // Startup takes exactly one of these: the persisted configuration if we have
  // one, otherwise the machine's defaults. The old single initializeAudio() ran
  // on both paths and overwrote everything loadSettings() had just restored.
  void applyLoadedAudio();
  void applyDefaultAudio();

  // The user picked a different API in the settings panel: rebuild the RtAudio
  // instance and take that API's default devices, since device ids from the
  // previous API mean nothing under the new one.
  void selectAudioApi(RtAudio::Api api);

  // Take the current API's default devices. Clears the persisted device names
  // (empty = system default) and derives the runtime used flags from the
  // persisted intent and a presence probe; the probe itself is never persisted.
  void selectDefaultDevices();
  void refreshDeviceChannelCounts();

  void startAudio();

  freeaudio::clap_wrapper::standalone::StandaloneHost *sah{
      freeaudio::clap_wrapper::standalone::getStandaloneHost()};

  std::vector<::COMDLG_FILTERSPEC> fileTypes{{L"clapwrapper", L"*.clapwrapper"}};

  bool isTimerRunning{false};
  const ::UINT_PTR timerId{0};

  // Written from RtAudio's error callback, which can be any thread.
  std::mutex pendingErrorLock;
  std::string pendingError;
  std::string lastShownError;

  ClapPlugin plugin;
  Position position;
  Menu menu;
  Settings settings;
};
}  // namespace freeaudio::clap_wrapper::standalone::windows_standalone
