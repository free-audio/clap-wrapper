#pragma once

/*
 * The Linux standalone's Audio/MIDI settings window, drawn on plain Xlib with x11_ui.h.
 * The same rows, rules and live apply as the macOS and Windows windows: every audio
 * choice opens at once, OK keeps it and saves, Cancel (or closing the window) goes back
 * to what ran when the window opened.
 */

#include "x11_ui.h"
#include "detail/standalone/standalone_host.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
class SettingsWindow
{
 public:
  SettingsWindow(Display *display, StandaloneHost *host, const UiFont &font, Window transientFor);
  ~SettingsWindow();

  // the window and its open menu, whose events go to handle()
  bool owns(Window w) const
  {
    return w != 0 && (w == window || w == popup);
  }
  void handle(const XEvent &event);
  void raise();

  // the host's devices or MIDI ports changed: the menus and lists follow, nothing opens
  void devicesChanged();
  // the host's appearance changed: drawn again in it
  void appearanceChanged();
  // from the frontend's poll: the latency note follows the stream
  void tick();
  // an audio error the host reports, shown in the window until the next choice
  void showError(const std::string &message);

  // set once the window has closed, with a clap_wrapper_standalone_configuration_result;
  // the frontend reports it to the plugin on the next turn of its loop
  bool closed() const
  {
    return closedWith.has_value();
  }
  uint32_t result() const
  {
    return closedWith.value_or(0);
  }

 private:
  // one control of the window. what a kind does not use stays empty
  struct Control
  {
    enum class Kind
    {
      Label,
      Heading,
      Note,
      Rule,
      Menu,
      Check,
      Button,
      PortList
    };
    Kind kind{Kind::Label};
    Rect rect;
    bool visible{true}, enabled{true};
    std::string text;
    unsigned long color{0};
    bool colored{false};

    // a menu's items and the selected one; tags for what an item stands for
    std::vector<std::string> items;
    std::vector<int64_t> tags;
    int selected{-1};

    // a checkbox
    bool checked{false};

    // a port list's rows
    struct Row
    {
      std::string reference, title;
      bool on{false}, wasOn{false}, connected{true};
    };
    std::vector<Row> rows;
    int scroll{0};
    std::string emptyText;

    std::function<void()> onChange;
  };

  struct AudioSetup
  {
    bool running{false};
    std::string inputId, outputId;
    bool useInput{false}, useOutput{false};
    int32_t rate{0};
    uint32_t frames{0};
    AudioChannelChoice inputChannels, outputChannels;
  };

  void build();
  void layout();
  void draw();
  void drawControl(const Painter &p, const Control &c, bool hot);
  void drawPopup();

  Control *hit(int x, int y);
  void click(Control &c, int x, int y);
  void openMenu(Control &menu);
  void closeMenu(bool choose);
  void scrollList(Control &list, int by);

  // the panel's steps, as AppDelegate.mm and windows_standalone.cpp take them
  void fillApis();
  void fillDevices(const std::string &output, const std::string &input);
  void fillChannels(uint32_t outputFirst, uint32_t inputFirst);
  void fillRates(uint32_t preferred);
  void fillBufferSizes(uint32_t preferred);
  void updateBufferTitles();
  void updateInputNote();
  void updateLatencyNote();
  void fillMidi();
  void fillAppearance();

  AudioChannelChoice shownChoice(bool input) const;
  const AudioDevice *shownDevice(bool input) const;
  uint32_t shownRate() const;
  uint32_t shownFrames() const;
  AudioSetup shownSetup() const;
  AudioSetup runningSetup() const;
  bool runsAs(const AudioSetup &shown) const;
  bool openSetup(const AudioSetup &setup);
  bool openShownAudio();
  void showRunningSetup();

  void onApiChanged();
  void onDeviceChanged();
  void onRateChanged();
  void onDefaults();
  void onAppearanceChanged();
  void ok();
  void close();

  Display *display;
  StandaloneHost *host;
  const UiFont &font;
  Theme theme;
  Window window{0}, popup{0};
  GC gc{nullptr};
  Atom wmDelete{0};
  int width{0}, height{0};

  // the menu that is open, and the item under the pointer in it
  Control *openedMenu{nullptr};
  int popupHot{-1}, popupScroll{0}, popupRows{0};
  Rect popupRect;
  Control *hot{nullptr};
  Control *pressed{nullptr};

  // the controls, in the order they are laid out
  Control heading, headingRule;
  Control apiLabel, api, outputLabel, output, outputChannelsLabel, outputChannels;
  Control inputLabel, input, inputChannelsLabel, inputChannels, inputNote;
  Control rateLabel, rate, bufferLabel, buffer, latencyNote, defaults, noRateNote, errorNote;
  Control midiRule, midiInLabel, midiIn, virtualIn, midiOutLabel, midiOut, virtualOut;
  Control appearanceRule, appearanceLabel, appearance;
  Control cancelButton, okButton;
  std::vector<Control *> order;

  std::vector<AudioApi> apis;
  std::vector<AudioDevice> outDevices, inDevices;
  std::vector<AudioChannelChoice> outChoices, inChoices;

  // what was so when the window opened, for Cancel
  AudioSetup audioWhenOpened;
  std::string apiWhenOpened;
  uint32_t appearanceWhenOpened{0};
  bool audioOpenedHere{false}, apiChanged{false};
  bool midiInAllOn{false}, virtualInWasOn{false}, virtualOutWasOn{false};
  bool takesMidi{false}, sendsMidi{false};
  std::optional<uint32_t> closedWith;
};
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
