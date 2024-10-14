#pragma once

#include <Windows.h>
#include <ShlObj.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <wil/com.h>
#include <wil/resource.h>

#include <winrt/windows.foundation.h>
#include <winrt/windows.data.json.h>

#define FMT_HEADER_ONLY
#include <fmt/format.h>
#include <fmt/xchar.h>

#include "detail/standalone/entry.h"
#include "detail/standalone/standalone_host.h"

namespace winrt
{
using namespace winrt::Windows::Data::Json;
};  // namespace winrt

namespace freeaudio::clap_wrapper::standalone::windows_standalone
{
std::pair<int, std::vector<std::string>> getArgs();
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

void log(const std::string& message);
void log(const std::wstring& message);

template <typename... Args>
void log(const fmt::format_string<Args...> fmt, Args&&... args)
{
  ::OutputDebugStringW(toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str());
  ::OutputDebugStringW(L"\n");
}

template <typename... Args>
void log(const fmt::wformat_string<Args...> fmt, Args&&... args)
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

  void box(const std::string& message);
  void box(const std::wstring& message);

  void error(const std::string& errorMessage);
  void error(const std::wstring& errorMessage);

  template <typename... Args>
  void box(const fmt::format_string<Args...> fmt, Args&&... args)
  {
    ::MessageBoxW(nullptr, toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str(),
                  nullptr, MB_OK | MB_ICONASTERISK);
  }

  template <typename... Args>
  void box(const fmt::wformat_string<Args...> fmt, Args&&... args)
  {
    ::MessageBoxW(nullptr, fmt::vformat(fmt.get(), fmt::make_wformat_args(args...)).c_str(), nullptr,
                  MB_OK | MB_ICONASTERISK);
  }

  template <typename... Args>
  void error(const fmt::format_string<Args...> fmt, Args&&... args)
  {
    ::MessageBoxW(nullptr, toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str(),
                  nullptr, MB_OK | MB_ICONHAND);
  }

  template <typename... Args>
  void error(const fmt::wformat_string<Args...> fmt, Args&&... args)
  {
    ::MessageBoxW(nullptr, fmt::vformat(fmt.get(), fmt::make_wformat_args(args...)).c_str(), nullptr,
                  MB_OK | MB_ICONHAND);
  }

 private:
  std::unordered_map<::UINT, MessageCallback> map;
};

struct Position
{
  uint32_t x{0};
  uint32_t y{0};
  uint32_t width{0};
  uint32_t height{0};
};

struct Window
{
  void create(const std::string& title);

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
  void create(const std::string& name, uintptr_t id, ::HWND parentHwnd);
  void reset();
  void add(const std::string& string);

  bool set(int index);
  bool set(const std::string& searchString);
  ::LRESULT get();
  ::LRESULT getItemHeight();

  static ::LRESULT CALLBACK procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam,
                                      ::UINT_PTR id, ::DWORD_PTR data);
};

struct ListBox final : public Control
{
  void create(const std::string& name, uintptr_t id, ::HWND parentHwnd);
  void reset();
  void add(const std::string& string);

  bool set(int index);
  bool set(const std::string& searchString);
  ::LRESULT get();
  ::LRESULT getItems(std::vector<int>& buffer);
  ::LRESULT getItemsCount();
  ::LRESULT getItemHeight();

  static ::LRESULT CALLBACK procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam,
                                      ::UINT_PTR id, ::DWORD_PTR data);
};

struct SystemMenu
{
  void add(std::wstring& name, ::UINT id);
  void addToggle(std::wstring& name, ::UINT id, bool checked = false);
  void addSeparator();
  void populate(::HWND hwnd);

  std::vector<::MENUITEMINFOW> item;
};

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

    template <typename T, typename U>
    auto set(std::string_view key, U value) -> void
    {
      if constexpr (std::is_same_v<T, std::string>)
      {
        json.SetNamedValue(toUTF16(key), winrt::JsonValue::CreateStringValue(toUTF16(value)));
      }

      if constexpr (std::is_same_v<T, bool>)
      {
        json.SetNamedValue(toUTF16(key), winrt::JsonValue::CreateBooleanValue(value));
      }

      if constexpr (std::is_same_v<T, double>)
      {
        json.SetNamedValue(toUTF16(key), winrt::JsonValue::CreateNumberValue(value));
      }

      if constexpr (std::is_same_v<T, Position>)
      {
        auto pos{winrt::JsonObject()};
        pos.SetNamedValue(L"x", winrt::JsonValue::CreateNumberValue(value.x));
        pos.SetNamedValue(L"y", winrt::JsonValue::CreateNumberValue(value.y));
        pos.SetNamedValue(L"width", winrt::JsonValue::CreateNumberValue(value.width));
        pos.SetNamedValue(L"height", winrt::JsonValue::CreateNumberValue(value.height));
        json.SetNamedValue(toUTF16(key), pos);
      }
    }

    template <typename T>
    auto get(std::string_view key) -> T
    {
      auto value{json.GetNamedValue(toUTF16(key), nullptr)};

      if constexpr (std::is_same_v<T, std::string>)
      {
        if (value && value.ValueType() == winrt::JsonValueType::String)
        {
          return toUTF8(value.GetString());
        }
        else
        {
          return {};
        }
      }

      if constexpr (std::is_same_v<T, bool>)
      {
        if (value && value.ValueType() == winrt::JsonValueType::Boolean)
        {
          return value.GetBoolean();
        }
        else
        {
          return false;
        }
      }

      if constexpr (std::is_same_v<T, double>)
      {
        if (value && value.ValueType() == winrt::JsonValueType::Number)
        {
          return value.GetNumber();
        }
        else
        {
          return 0.0;
        }
      }

      if constexpr (std::is_same_v<T, Position>)
      {
        if (value && value.ValueType() == winrt::JsonValueType::Object)
        {
          auto parsedPos{value.GetObject()};

          Position buffer;
          buffer.x = static_cast<uint32_t>(parsedPos.GetNamedNumber(L"x"));
          buffer.y = static_cast<uint32_t>(parsedPos.GetNamedNumber(L"y"));
          buffer.width = static_cast<uint32_t>(parsedPos.GetNamedNumber(L"width"));
          buffer.height = static_cast<uint32_t>(parsedPos.GetNamedNumber(L"height"));

          return buffer;
        }
        else
        {
          return {};
        }
      }
    }

    ComboBox api;
    ComboBox output;
    ComboBox input;
    ComboBox sampleRate;
    ComboBox bufferSize;
    ListBox midiIn;

    winrt::JsonObject json;
  };

  struct ClapPlugin
  {
    std::shared_ptr<Clap::Plugin> clap;
    const clap_plugin_t* plugin;
    const clap_plugin_gui_t* gui;
    const clap_plugin_state_t* state;
  };

  explicit Plugin(std::shared_ptr<Clap::Plugin> clapPlugin);

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

  void initializeMIDI();
  void startMIDI();

  void initializeAudio(RtAudio::Api api = RtAudio::Api::WINDOWS_WASAPI);
  void startAudio();

  freeaudio::clap_wrapper::standalone::StandaloneHost* sah{
      freeaudio::clap_wrapper::standalone::getStandaloneHost()};

  std::vector<::COMDLG_FILTERSPEC> fileTypes{{L"clapwrapper", L"*.clapwrapper"}};

  bool isTimerRunning{false};
  const ::UINT_PTR timerId{0};

  ClapPlugin plugin;
  Position position;
  Menu menu;
  Settings settings;
};
}  // namespace freeaudio::clap_wrapper::standalone::windows_standalone
