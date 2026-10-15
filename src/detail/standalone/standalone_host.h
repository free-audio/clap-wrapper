#pragma once

#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <optional>
#include <tuple>

#include "standalone_details.h"
#include "standalone_settings.h"

#include "detail/clap/fsutil.h"

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtAudio.h"
#include "RtMidi.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "clap_proxy.h"
#include "detail/shared/fixedqueue.h"
#include "detail/shared/spinlock.h"

namespace freeaudio::clap_wrapper::standalone
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
namespace linux_standalone
{
struct X11Gui;
}
#endif

std::optional<fs::path> getStandaloneSettingsPath();

// the inputs a launch with no saved settings should open in place of the system default
// input, best first, as rtaudio names them; empty keeps the system default. on macOS that
// is an audio interface (usb, thunderbolt, firewire, pci or avb) when the system default
// is a built-in, bluetooth or iphone microphone. elsewhere there is no way to tell yet,
// and the answer is always empty
std::vector<std::string> preferredFirstRunInputs();

// whether the device of this rtaudio name is a microphone: the built-in one, a bluetooth
// headset's or speaker's, or an iphone's. the standalone opens one only when the user
// chose it by name, never as a default or a fallback: a built-in microphone picks up the
// speakers beside it, and opening a bluetooth device's switches the whole device to its
// hands-free profile - telephone quality and one sample rate, for every application on
// the machine. on windows, an endpoint whose form factor is a microphone, headset or
// handset. false where there is no way to tell, which is linux for now
bool isMicrophoneAudioDevice(const std::string &rtAudioName);

// whether the device of this rtaudio name is a bluetooth device; false where there is no
// way to tell
bool isBluetoothAudioDevice(const std::string &rtAudioName);

// the smallest and largest buffer, in frames, the device of this rtaudio name accepts;
// {0, 0} where there is no way to tell, which is everywhere but macos for now. rtaudio
// clamps a request to the range without a word, so a menu should not offer beyond it
std::pair<uint32_t, uint32_t> bufferFrameSizeRange(const std::string &rtAudioName);

// the clap_wrapper_standalone_appearance values the configuration screen can take, as
// bits (1 << appearance); 0 where the frontend cannot set one
uint32_t supportedScreenAppearances();

struct StandaloneHost : Clap::IHost
{
  StandaloneHost()
  {
    inputEvents.ctx = this;
    inputEvents.size = ie_getsize;
    inputEvents.get = ie_get;
    outputEvents.ctx = this;
    outputEvents.try_push = oe_try_push;
  }
  virtual ~StandaloneHost();

  static bool oe_try_push(const struct clap_output_events *oe, const clap_event_header_t *evt)
  {
    auto sh = (StandaloneHost *)oe->ctx;
    return sh->outputEventPush(evt);
  }

  static uint32_t ie_getsize(const struct clap_input_events *ie)
  {
    auto sh = (StandaloneHost *)ie->ctx;
    return sh->inputEventSize();
  }
  static const clap_event_header_t *ie_get(const struct clap_input_events *ie, uint32_t idx)
  {
    auto sh = (StandaloneHost *)ie->ctx;
    return sh->inputEvent(idx);
  }

  static constexpr int maxEventsPerCycle{256};
  static constexpr int eventSize{1024};
  static constexpr int queueSize{eventSize * maxEventsPerCycle};
  const unsigned char eventQueue[queueSize]{};

  int currInput{0};
  void clearInputEvents()
  {
    currInput = 0;
  }
  bool pushInputEvent(clap_event_header_t *event)
  {
    if (event->size > eventSize)
    {
      return false;
    }
    if (currInput >= maxEventsPerCycle)
    {
      return false;
    }
    memcpy((void *)(eventQueue + currInput * eventSize), (const void *)event, event->size);
    currInput++;
    return true;
  }
  uint32_t inputEventSize()
  {
    return currInput;
  }
  const clap_event_header_t *inputEvent(uint32_t idx)
  {
    return (const clap_event_header_t *)(eventQueue + idx * eventSize);
  }

  std::shared_ptr<Clap::Plugin> clapPlugin;
  void setPlugin(std::shared_ptr<Clap::Plugin> plugin)
  {
    this->clapPlugin = plugin;
  }

  void mark_dirty() override
  {
    TRACE;
  }
  std::atomic<bool> restartRequested{false};
  void restartPlugin() override
  {
    restartRequested = true;
  }
  std::atomic<bool> processRequested{false};
  void request_process() override
  {
    processRequested = true;
  }
  std::atomic<bool> callbackRequested{false};
  void request_callback() override
  {
    callbackRequested = true;
  }
  void setupWrapperSpecifics(const clap_plugin_t *plugin) override
  {
    TRACE;
  }

  bool saveStandaloneAndPluginSettings(const fs::path &intoDir, const fs::path &withName);
  bool tryLoadStandaloneAndPluginSettings(const fs::path &fromDir, const fs::path &withName);

  // The audio/MIDI configuration, as opposed to the plugin state blob above.
  // Frontends read and write this rather than rolling their own store; see
  // standalone_settings.h for the format and for why devices are held by name.
  StandaloneSettings settings;
  bool settingsLoaded{false};

  // <settings path>/<plugin id>/standalone-settings.conf, or nullopt when the
  // platform has no settings path or no plugin is loaded yet.
  std::optional<fs::path> standaloneSettingsFile();
  bool loadStandaloneSettings();
  bool saveStandaloneSettings();

  // Current audio configuration -> settings, and back. applyAudioSettings()
  // selects the API and resolves the persisted device *names* against the
  // devices this machine actually has right now.
  // captureAudioSettings() writes back only the API and buffer size; the device
  // names, used flags and sample rate are the user's request, written by the frontend.
  void captureAudioSettings();
  void applyAudioSettings();

  // Device id for a persisted name, falling back to the system default when the
  // name is empty or no longer present. Never throws, never fails.
  unsigned int resolveInputDevice(const std::string &name);
  unsigned int resolveOutputDevice(const std::string &name);

  // True when RtAudio's current enumeration contains this id. Ask before calling
  // getDeviceInfo(): an unknown id raises an error through the error callback.
  bool isKnownDevice(unsigned int deviceID);
  std::string deviceName(unsigned int deviceID);
  RtAudio::DeviceInfo deviceInfoFor(unsigned int deviceID);

  // Enumerating is not cheap and is not side-effect free. RtAudio's ASIO backend
  // loads, ASIOInit()s and unloads *every* installed ASIO driver on each probe,
  // and some drivers do not survive that being done repeatedly - the Generic Low
  // Latency driver starts failing to allocate buffers. So the device list is
  // enumerated once and reused until something happens that could change it.
  const std::vector<RtAudio::DeviceInfo> &deviceList();
  void invalidateDeviceList()
  {
    deviceListValid = false;
  }

 private:
  std::vector<RtAudio::DeviceInfo> deviceListCache;
  bool deviceListValid{false};

 public:
  uint32_t numAudioInputs{0}, numAudioOutputs{0};
  std::vector<uint32_t> inputChannelByBus;
  std::vector<uint32_t> outputChannelByBus;
  uint32_t mainInput{0}, mainOutput{0};
  uint32_t totalInputChannels{0}, totalOutputChannels{0};
  void setupAudioBusses(const clap_plugin_t *plugin,
                        const clap_plugin_audio_ports_t *audioports) override;

  bool hasMIDIInput{false}, hasClapNoteInput{false}, createsMidiOutput{false};
  void setupMIDIBusses(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *noteports) override;

  void setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params) override
  {
    TRACE;
  }
  void param_rescan(clap_param_rescan_flags flags) override
  {
    TRACE;
  }
  void param_clear(clap_id param, clap_param_clear_flags flags) override
  {
    TRACE;
  }
  void param_request_flush() override
  {
  }

  bool gui_can_resize() override;

  std::function<bool(uint32_t, uint32_t)> onRequestResize = [](auto, auto) { return false; };
  bool gui_request_resize(uint32_t width, uint32_t height) override;

  bool gui_request_show() override
  {
    TRACE;
    return false;
  }
  bool gui_request_hide() override
  {
    TRACE;
    return false;
  }

#if LIN && CLAP_WRAPPER_STANDALONE_X11
  freeaudio::clap_wrapper::standalone::linux_standalone::X11Gui *x11Gui{nullptr};
#endif

  bool register_timer(uint32_t period_ms, clap_id *timer_id) override;
  bool unregister_timer(clap_id timer_id) override;

  const char *host_get_name() override;
  const char *wrapper_flavor() const override
  {
    return CLAP_WRAPPER_HOST_FLAVOR_STANDALONE;
  }
  const char *underlying_host_name() const override
  {
#if MAC
    return "Wrapper Mac Standalone";
#elif WIN
    return "Wrapper Windows Standalone";
#else
    return "Wrapper Linux Standalone";
#endif
  }

  bool track_info_get(clap_track_info_t *info) override
  {
    return false;
  }

  // standalone features extension, in standalone_host_features.cpp
  const clap_wrapper_standalone_features_t *standalone_features() override;

  // set by the platform frontend; null means it has no settings screen to offer
  std::function<bool()> onShowSettingsScreen{nullptr};

  // called by the platform frontend once per close of its settings screen, after the
  // screen is gone; passes it on to the plugin's side of the standalone features
  // extension, if it has one. result is a clap_wrapper_standalone_configuration_result
  void settingsScreenClosed(uint32_t result);

  // the settings screen's appearance, a clap_wrapper_standalone_appearance. a plugin that
  // sets it owns it for the session: the settings file and the screen's own choice no
  // longer count, and the plugin's value is never saved
  uint32_t screenAppearance{CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO};
  bool screenAppearanceFromPlugin{false};
  // false if the appearance is unsupported, or the plugin owns it and this is not it
  bool setScreenAppearance(uint32_t appearance, bool fromPlugin);
  // set by the platform frontend, which applies screenAppearance to what it shows
  std::function<void()> onScreenAppearanceChanged{nullptr};

  std::vector<clap_wrapper_standalone_endpoint_t> endpointSnapshot[4];
  uint32_t snapshotEndpoints(uint32_t kind);
  std::vector<uint32_t> sampleRateSnapshot;
  uint32_t snapshotSampleRates(const char *outputId, const char *inputId);
  std::vector<uint32_t> bufferSizeSnapshot;
  uint32_t snapshotBufferSizes(const char *outputId, const char *inputId);

  // context menu extension
  bool supportsContextMenu() const override
  {
    return false;
  }
  bool context_menu_populate(const clap_context_menu_target_t *target,
                             const clap_context_menu_builder_t *builder) override
  {
    return false;
  }
  bool context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id) override
  {
    return false;
  }
  bool context_menu_can_popup() override
  {
    return false;
  }
  bool context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index, int32_t x,
                          int32_t y) override
  {
    return false;
  }

#if LIN
  bool register_fd(int fd, clap_posix_fd_flags_t flags) override;
  bool modify_fd(int fd, clap_posix_fd_flags_t flags) override;
  bool unregister_fd(int fd) override;

#endif

  void latency_changed() override
  {
    TRACE;
  }
  void tail_changed() override
  {
    TRACE;
  }

  // Implementation in standalone_host_midi.cpp
  struct midiChunk
  {
    char dat[3]{};
    midiChunk()
    {
      memset(dat, 0, sizeof(dat));
    }
  };
  ClapWrapper::detail::shared::fixedqueue<midiChunk, 4096> midiToAudioQueue;
  // the queue takes one producer at a time, and rtmidi may call each open input back on
  // a thread of its own
  ClapWrapper::detail::shared::SpinLock midiToAudioPushLock;
  std::vector<std::unique_ptr<RtMidiIn>> midiIns;
  uint32_t numMidiPorts{0};
  std::vector<uint32_t> currentMidiPorts;
  // indices go stale on a replug; names do not
  std::vector<std::string> currentMidiPortNames;
  // Port names as the MIDI system reports them right now; also refreshes
  // numMidiPorts. Returns empty (rather than throwing or exiting) when there is
  // no usable MIDI system.
  std::vector<std::string> getMidiPortNames();

  // Close whatever is open and open exactly these, matched by name. bindAll
  // ignores the list and opens everything - which is a different thing from an
  // empty list, that being a user who deliberately wants no MIDI input.
  void openMidiPorts(const std::vector<std::string> &names, bool bindAll);
  void closeMidiPorts();

  // Virtual ports: a destination other applications send to, which feeds the plugin like
  // an input, and a source they receive the plugin's output from. Each lives on its own,
  // so reopening the port lists never makes another application lose its connection.
  // Named after the plugin, with a suffix - Kemper's hardware, for one, has a port named
  // like its plugin - and left out of the port lists, so that binding every input can
  // never loop the plugin's output back into it.
  static bool supportsVirtualMidiPorts();
  std::string virtualMidiPortName(bool input) const;
  std::unique_ptr<RtMidiIn> midiVirtualIn;
  std::unique_ptr<RtMidiOut> midiVirtualOut;
  void setVirtualMidiInput(bool open);
  void setVirtualMidiOutput(bool open);

  // MIDI output. what the plugin sends from its note output ports goes to every open
  // output port: the audio thread queues it in outputEventPush() and a thread of its own
  // sends it, since an rtmidi send is no call to make from the audio thread
  ClapWrapper::detail::shared::fixedqueue<midiChunk, 4096> midiFromAudioQueue;
  std::vector<std::unique_ptr<RtMidiOut>> midiOuts;
  uint32_t numMidiOutputPorts{0};
  std::vector<std::string> currentMidiOutputPortNames;
  // set while midiOutputThread runs; the audio thread queues nothing while it is clear
  std::atomic<bool> midiOutputOpen{false};
  std::thread midiOutputThread;
  // the thread runs while there is a port to send to, the virtual output included
  void stopMidiOutputThread();
  void startMidiOutputThreadIfNeeded();

  // as getMidiPortNames(), for outputs; refreshes numMidiOutputPorts
  std::vector<std::string> getMidiOutputPortNames();

  // Close whatever outputs are open and open exactly these, matched by name. Unlike the
  // inputs there is no bind-all: an empty list opens none, which is also the first run,
  // since sending to every port would play into whatever is plugged in. A plugin with no
  // MIDI output opens none either - on Windows a MIDI port is held exclusively.
  void openMidiOutputPorts(const std::vector<std::string> &names);
  // closes every output, the virtual one included
  void closeMidiOutputPorts();
  void midiOutputLoop();

  // [audio-thread] the plugin's output events. MIDI and note on/off events go to the MIDI
  // outputs, anything else is taken and dropped
  bool outputEventPush(const clap_event_header_t *evt);

  // inputs and outputs, from the settings
  void startMIDIThread();
  void stopMIDIThread();
  void processMIDIEvents(double deltatime, std::vector<unsigned char> *message);
  static void midiCallback(double deltatime, std::vector<unsigned char> *message, void *userData);

  // in standalone_host.cpp
  void clapProcess(void *pOutput, const void *pInoput, uint32_t frameCount);
  // samples processed so far; never reset, so it stays monotonic across restarts
  int64_t steadyTime{0};

  // Actual audio IO In standalone_host_audio.cpp
  std::unique_ptr<RtAudio> rtaDac;
  std::function<void(const std::string &)> displayAudioError{nullptr};
  // why the most recent startAudioThreadOn failed; empty if it succeeded
  std::string lastAudioError;

  // a stream is open and the plugin is processing it. startAudioThreadOn() settles this
  // before it returns, so it answers whether that open worked
  bool isAudioRunning() const
  {
    return isActive && (currentOutputChannels > 0 || currentInputChannels > 0);
  }
  // session only: a launch never starts muted on a previous session's say-so
  std::atomic<bool> inputMuted{false};

  // the audio driver's own settings window, which only asio has: whether this standalone
  // can show one for that api, which needs rtaudio built with asio
  static bool hasDriverControlPanel(RtAudio::Api api);
  // shows it for the stream open now; false if no asio stream is open
  bool showDriverControlPanel();

  // an asio driver asks for a reset when its own panel changed the buffer size or the
  // rate, and rtaudio closes the stream for it, from a thread of its own. true, once, when
  // that stream is closed: the frontend then opens it again, on the main thread
  bool takeDriverReset();
  std::atomic<bool> driverResetRequested{false};

  // RtAudio reports enumeration failures through the same error callback it uses
  // for stream failures, and we enumerate every time a settings panel refreshes.
  // Those are not events to put in front of the user, so the enumeration helpers
  // below silence the dialog for their duration. Logging is never silenced.
  std::atomic<int> audioErrorSilence{0};
  bool audioErrorsAreSilent() const
  {
    return audioErrorSilence.load() > 0;
  }
  struct SilenceAudioErrors
  {
    StandaloneHost *host;
    explicit SilenceAudioErrors(StandaloneHost *h) : host(h)
    {
      host->audioErrorSilence.fetch_add(1);
    }
    ~SilenceAudioErrors()
    {
      host->audioErrorSilence.fetch_sub(1);
    }
  };
  RtAudio::Api audioApi{RtAudio::Api::UNSPECIFIED};
  std::string audioApiName{RtAudio::getApiName(RtAudio::Api::UNSPECIFIED)};
  std::string audioApiDisplayName{RtAudio::getApiDisplayName(RtAudio::Api::UNSPECIFIED)};
  unsigned int audioInputDeviceID{0}, audioOutputDeviceID{0};
  // The user's wish (settings.audioInput/OutputUsed) AND a device being present;
  // the probe half is why these are not persisted.
  bool audioInputUsed{true}, audioOutputUsed{true};
  // How many channels to ask the *device* for. Distinct from
  // totalInput/OutputChannels above, which are the plugin's bus totals.
  uint32_t deviceInputChannels{2}, deviceOutputChannels{2};
  int32_t currentSampleRate{0};
  uint32_t currentBufferSize{0};
  uint32_t currentInputChannels{0}, currentOutputChannels{0};
  void guaranteeRtAudioDAC();
  void setAudioApi(RtAudio::Api api);
  std::tuple<unsigned int, unsigned int, int32_t> getDefaultAudioInOutSampleRate();
  void startAudioThread();
  void startAudioThreadOn(unsigned int inputDeviceID, uint32_t inputChannels, bool useInput,
                          unsigned int outputDeviceID, uint32_t outputChannels, bool useOutput,
                          int32_t sampleRate);
  void stopAudioThread();

  // Park the audio callback: clear running under processLock, then wait for the
  // callback to acknowledge. Returns false if the acknowledgement never came.
  // Callers which are about to touch plugin state must go through this.
  bool quiesceProcessing();

  // the actual work of startAudioThreadOn, wrapped there by an exception guard
  void startAudioThreadOnImpl(unsigned int inputDeviceID, uint32_t inputChannels, bool useInput,
                              unsigned int outputDeviceID, uint32_t outputChannels, bool useOutput,
                              int32_t sampleRate);

  // returns true if the plugin is activated and processing afterwards
  bool activatePlugin(int32_t sr, int32_t minBlock, int32_t maxBlock);
  // no-op unless activatePlugin() succeeded before, so the plugin never sees
  // an unbalanced deactivate()
  void deactivatePlugin();
  bool isActive{false};
  bool isProcessing{false};

  std::vector<RtAudio::Api> getCompiledApi();
  std::vector<RtAudio::DeviceInfo> getInputAudioDevices();
  std::vector<RtAudio::DeviceInfo> getOutputAudioDevices();
  std::vector<int32_t> getSampleRates();
  std::vector<uint32_t> getBufferSizes();
  // getBufferSizes(), narrowed to what both devices accept where the platform can tell
  // (bufferFrameSizeRange); an empty name narrows nothing
  std::vector<uint32_t> getBufferSizesFor(const std::string &outputName, const std::string &inputName);

  clap_input_events inputEvents{};
  clap_output_events outputEvents{};

  ClapWrapper::detail::shared::SpinLock processLock;
  std::atomic<bool> running{true}, finishedRunning{false};

  // We need to have play buffers for the clap. For now lets assume
  // (1) the standalone is never more than 64 total ins and outs and
  // (2) the block size is less that 4096 * 16 and
  // (3) memory in the standalone is pretty cheap
  static constexpr int utilityBufferSize{4096 * 16};
  static constexpr int utilityBufferMaxChannels{64};
  float utilityBuffer[utilityBufferMaxChannels][utilityBufferSize]{};
};
}  // namespace freeaudio::clap_wrapper::standalone
