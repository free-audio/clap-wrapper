#pragma once

#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>

#include "standalone_details.h"

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

namespace freeaudio::clap_wrapper::standalone
{
#if LIN
#if CLAP_WRAPPER_HAS_GTK3
namespace linux
{
struct GtkGui;
}
#endif
#endif

#if WIN
#if CLAP_WRAPPER_HAS_WIN32
namespace windows
{
struct Win32Gui;
}
#endif
#endif

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
    return true;
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
      LOG << "BAD EVENT" << std::endl;
      return false;
    }
    if (currInput >= maxEventsPerCycle)
    {
      LOG << "TOO MANY EVENTS" << std::endl;
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
  void restartPlugin() override
  {
    TRACE;
  }
  void request_callback() override
  {
    TRACE;
  }
  void setupWrapperSpecifics(const clap_plugin_t *plugin) override
  {
    TRACE;
  }

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
  bool gui_can_resize() override
  {
    TRACE;
    return false;
  }
  bool gui_request_resize(uint32_t width, uint32_t height) override
  {
    TRACE;
    return false;
  }
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

#if LIN
#if CLAP_WRAPPER_HAS_GTK3
  freeaudio::clap_wrapper::standalone::linux::GtkGui *gtkGui{nullptr};
#endif
#endif

#if WIN
#if CLAP_WRAPPER_HAS_WIN32
  freeaudio::clap_wrapper::standalone::windows::Win32Gui *win32Gui{nullptr};
#endif
#endif

  bool register_timer(uint32_t period_ms, clap_id *timer_id) override;
  bool unregister_timer(clap_id timer_id) override;
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
  std::vector<std::unique_ptr<RtMidiIn>> midiIns;
  void startMIDIThread();
  void stopMIDIThread();
  void processMIDIEvents(double deltatime, std::vector<unsigned char> *message);
  static void midiCallback(double deltatime, std::vector<unsigned char> *message, void *userData);

  // in standalone_host.cpp
  void clapProcess(void *pOutput, const void *pInoput, uint32_t frameCount);

  // In standalone_host_audio.cpp
  std::unique_ptr<RtAudio> rtaDac;
  void startAudioThread();
  void stopAudioThread();

  clap_input_events inputEvents{};
  clap_output_events outputEvents{};

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
