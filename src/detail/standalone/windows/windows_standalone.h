#pragma once

#include <Windows.h>
#include <ShlObj.h>

#include <chrono>
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
// the font Windows uses in its own dialogs (Segoe UI), for a window at that dpi; percent
// scales its height and weight is a FW_ value, 0 the font's own
::HFONT getMessageFont(uint32_t dpi, int percent = 100, int weight = 0);
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

  // the window a box is shown over and holds while it is up; none until the window is
  // made, and a box without one can open behind it
  ::HWND owner{nullptr};

  void box(const std::string &message);
  void box(const std::wstring &message);

  void error(const std::string &errorMessage);
  void error(const std::wstring &errorMessage);

  template <typename... Args>
  void box(const fmt::format_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(owner, toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str(),
                  nullptr, MB_OK | MB_ICONASTERISK);
  }

  template <typename... Args>
  void box(const fmt::wformat_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(owner, fmt::vformat(fmt.get(), fmt::make_wformat_args(args...)).c_str(), nullptr,
                  MB_OK | MB_ICONASTERISK);
  }

  template <typename... Args>
  void error(const fmt::format_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(owner, toUTF16(fmt::vformat(fmt.get(), fmt::make_format_args(args...))).c_str(),
                  nullptr, MB_OK | MB_ICONHAND);
  }

  template <typename... Args>
  void error(const fmt::wformat_string<Args...> fmt, Args &&...args)
  {
    ::MessageBoxW(owner, fmt::vformat(fmt.get(), fmt::make_wformat_args(args...)).c_str(), nullptr,
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
  void create(::LPCWSTR className, const std::string &text, ::DWORD style, uintptr_t id,
              ::HWND parentHwnd, ::DWORD exStyle = 0);

  void setPosition(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
  void setFont(::HFONT font);
  void setText(const std::string &text);
  // hidden controls take no room in the settings window's layout
  void setVisible(bool visible);
  bool visible{true};

  Position position;

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

// a list of midi ports, one checkbox each. ports saved but not plugged in now are listed
// too, marked, so that unticking one is a way to forget it
struct PortList final : public Control
{
  struct Row
  {
    // the port's id, or for a port saved but not plugged in, its saved name
    std::string reference;
    bool wasOn;  // when the settings window opened
  };

  void create(uintptr_t id, ::HWND parentHwnd);
  // one row per port, ticked if its id is in onIds, or every port with allOn, and one
  // marked "(not connected)" per name in absentNames. a row already there keeps its
  // state, so that a port coming or going while the window is open changes nothing else
  void fill(const std::vector<MidiPort> &ports, const std::vector<std::string> &onIds,
            const std::vector<std::string> &absentNames, bool allOn);

  bool isOn(size_t row);
  void setOn(size_t row, bool on);
  // a list nobody touched leaves its settings as they were, which for the inputs may be
  // "every port", including any plugged in later
  bool changed();
  std::vector<std::string> ticked();

  // the checkbox images come from the theme the list has when they are made, or are
  // drawn, for the dark, with the frame colour for their box
  void setTheme(bool dark, ::COLORREF background, ::COLORREF text, ::COLORREF frame);

  // the height of one row, from the font the list has now
  uint32_t rowHeight();

  std::vector<Row> rows;

  // shown in the list's place while it has no rows
  Control empty;
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

// Posted to the plugin window once the settings window has been hidden, so the plugin
// hears about it from the main loop. wparam is 1 if OK applied its choice.
constexpr ::UINT WM_SETTINGS_CLOSED{WM_APP + 2};

// What the standalone features extension calls to show the settings window, installed
// before the plugin is created. False until the plugin window exists.
bool showSettingsScreen();

struct Plugin final : public Window
{
  struct Menu final : public SystemMenu
  {
    enum Identifier
    {
      AudioMidiSettings,
      SaveState,
      LoadState,
      ResetState,
      RestoreLastSession
    };

    std::wstring audioMidiSettings{L"Audio/MIDI Settings"};
    std::wstring saveState{L"Save state..."};
    std::wstring loadState{L"Load state..."};
    std::wstring resetState{L"Reset state"};
    std::wstring restoreLastSession{L"Restore last session"};
  };

  // the colours the settings window is drawn in, for its appearance
  struct Palette
  {
    bool dark{false};
    ::COLORREF background;
    ::COLORREF text;
    ::COLORREF secondary;
    ::COLORREF critical;
    ::COLORREF caution;
    ::COLORREF rule;
    ::COLORREF field;  // lists and drop-down lists
  };

  struct Settings final : public Window
  {
    // OK and Cancel are IDOK and IDCANCEL, which is what Enter and Esc send
    enum Identifier
    {
      AudioApi = 100,
      AudioOutput,
      AudioInput,
      AudioSamplerate,
      AudioBuffersize,
      ResetToDefault,
      MidiInputs,
      MidiOutputs,
      VirtualInput,
      VirtualInputLabel,
      VirtualOutput,
      VirtualOutputLabel,
      Appearance,
      AudioOutputChannels,
      AudioInputChannels,
      DriverControlPanel,
      Static
    };

    Control heading;
    Control apiLabel, outputLabel, inputLabel, sampleRateLabel, bufferSizeLabel;
    // which of the device's channels the plugin's main ports take
    Control outputChannelsLabel, inputChannelsLabel;
    ComboBox outputChannels, inputChannels;
    ComboBox api;
    ComboBox output;
    ComboBox input;
    Control inputNote;
    ComboBox sampleRate;
    ComboBox bufferSize;
    Control resetToDefault;
    Control driverPanel;  // the asio driver's control panel
    Control noRateNote;
    // beside the buffer size: what the running stream's device adds, while the window
    // shows what runs
    Control latencyNote;

    Control midiInLabel, midiOutLabel;
    PortList midiIn;
    PortList midiOut;
    Control virtualIn, virtualInLabel;
    Control virtualOut, virtualOutLabel;

    Control appearanceLabel;
    ComboBox appearance;

    Control ok;
    Control cancel;

    wil::unique_hfont font;
    wil::unique_hfont headingFont;
    Palette palette;
    wil::unique_hbrush backgroundBrush;
    wil::unique_hbrush fieldBrush;

    // where layoutSettings() put the horizontal rules, which WM_PAINT draws
    std::vector<::RECT> rules;

    // what the window shows, in the order it shows it: what a selection there indexes
    std::vector<standalone::AudioApi> apisShown;
    std::vector<AudioDevice> outDevices, inDevices;  // the input list adds "No Input" first
    std::vector<AudioChannelChoice> outChoices, inChoices;
    std::vector<int32_t> sampleRatesShown;
    std::vector<uint32_t> bufferSizesShown;
    std::vector<uint32_t> appearancesShown;

    // as they were when the window was shown, to go back to
    std::string apiWhenOpened;
    // the input list's "every port" from the settings, for ports plugged in while it is open
    bool midiInAllOn{false};
    bool apiChanged{false};
    // a stream was opened before OK, for the driver's control panel
    bool audioOpenedEarly{false};
    // inside the driver's control panel call, which a driver reset must wait for
    bool inDriverPanel{false};
    uint32_t appearanceWhenOpened{0};
    bool virtualInWasOn{false};
    bool virtualOutWasOn{false};

    // the input note is a warning, not only a remark
    bool inputNoteCaution{false};
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

  // the settings window: built afresh from the host each time it is shown, laid out to
  // fit what it shows, and applied only by OK
  void createSettingsControls();
  void populateSettings();
  void layoutSettings();
  void refreshSettingsFonts();
  void refreshApis();
  void refreshDevices(const std::string &outputId, const std::string &inputId, bool useInput);
  // a device or port came or went: the window, if it is up, follows and keeps what it shows
  void devicesChanged();
  void refreshSampleRates(int32_t preferred);
  void refreshBufferSizes(uint32_t preferred);
  void refreshBufferSizeTitles();
  // the channel menus for the shown devices, selecting the choice that starts at these
  // channels (0-based), or the first; the input's row only while there is an input
  void refreshChannelChoices(uint32_t outputFirst, uint32_t inputFirst);
  void refreshInputNote();
  void refreshLatencyNote();
  void refreshAppearanceRow();
  void applySettingsAppearance();
  void selectApiInSettings();
  void resetSettingsToDefault();
  void refreshDriverPanelButton();
  void showDriverControlPanel();
  bool openShownAudio();
  // a menu changed: what the window shows is opened at once, or the menus go back to
  // what runs
  void applyShownAudio();
  void okSettings();
  void cancelSettings();

  // the selections the window shows; an input of nullopt is "No Input"
  std::optional<size_t> shownOutput();
  std::optional<size_t> shownInput();
  int32_t shownSampleRate();
  uint32_t shownBufferSize();
  // the channels a channel menu shows; {0, 0} with none
  AudioChannelChoice shownChannels(bool input);

  // the plugin has a note port in that direction; the window has no section without one
  bool takesMidi() const;
  bool sendsMidi() const;

  bool saveSettings();
  bool loadSettings();

  void startMIDI();

  // Startup takes exactly one of these: the persisted configuration if we have
  // one, otherwise the machine's defaults. The old single initializeAudio() ran
  // on both paths and overwrote everything loadSettings() had just restored.
  void applyLoadedAudio();
  void applyDefaultAudio();

  // Take the current API's default devices; clears the persisted device names.
  void selectDefaultDevices();

  void startAudio();

  // the system menu's Restore last session: flips the choice, and shows it in the menu
  void toggleRestoreLastSession();
  void showRestoreLastSession(::HMENU systemMenu);

  // The settings window, as the system menu toggles it and the standalone features
  // extension shows it. showSettings() brings it forward when it is already up;
  // hideSettings() reports the close to the plugin, applied if settingsApplied, which
  // OK sets. Cancel, Esc and the close button go through cancelSettings() first.
  void showSettings();
  void hideSettings();
  bool settingsApplied{false};

  freeaudio::clap_wrapper::standalone::StandaloneHost *sah{
      freeaudio::clap_wrapper::standalone::getStandaloneHost()};

  std::vector<::COMDLG_FILTERSPEC> fileTypes{{L"clapwrapper", L"*.clapwrapper"}};

  bool isTimerRunning{false};
  const ::UINT_PTR timerId{0};

  // Written by StandaloneHost::displayAudioError, which may be inside a handler that is
  // still on the stack.
  std::mutex pendingErrorLock;
  std::string pendingError;
  std::string lastShownError;
  // an error box is up: its modal loop runs the timer, which must not open another
  bool showingAudioError{false};
  // since when the stream has run without stopping; a device that was just plugged in
  // can stop a stream again within moments, which is still the failure already shown
  std::optional<std::chrono::steady_clock::time_point> audioRunningSince;

  ClapPlugin plugin;
  Position position;
  Menu menu;
  Settings settings;
};
}  // namespace freeaudio::clap_wrapper::standalone::windows_standalone
