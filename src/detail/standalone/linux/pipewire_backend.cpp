/*
 * The standalone's PipeWire backend, with libpipewire loaded at run time: a build has it
 * whether or not the machine it runs on has PipeWire, and without the library or a running
 * server it lists no devices and says so when a stream is opened.
 *
 * A device is an Audio/Sink or Audio/Source node: its id is the node's name, which stays
 * the same across launches and replugs, its name the node's description, which is also
 * what RtAudio's pulse api called it through pipewire-pulse. The defaults are the "default"
 * metadata's, the rates and buffer sizes the "settings" metadata's clock: a stream asks
 * for its rate (node.rate) and its buffer (node.latency) and never forces the graph.
 *
 * A stream is one pw_filter with an input port per input channel and an output port per
 * output channel, so that input and output run in one graph cycle and one process() call,
 * linked here to the chosen device's ports from the chosen channel on. A cycle larger
 * than the buffer the stream was opened with is split into blocks of that size.
 *
 * One thread loop, context and core for the backend's lifetime; open() and stop() make
 * and take down only the filter and its links.
 *
 * Nothing in here throws.
 */

#include "detail/standalone/standalone_backends.h"
#include "detail/standalone/standalone_details.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/link.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/dict.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// libpipewire's functions, from the library dlopen()ed once per process. the declarations
// in its headers give their types; nothing here links against them. what the headers
// define inline (the core, registry, node and metadata methods, the loop's) needs nothing
struct PipeWire
{
  decltype(&::pw_init) init{nullptr};
  decltype(&::pw_loop_new) loop_new{nullptr};
  decltype(&::pw_loop_destroy) loop_destroy{nullptr};
  decltype(&::pw_thread_loop_new) thread_loop_new{nullptr};
  decltype(&::pw_thread_loop_destroy) thread_loop_destroy{nullptr};
  decltype(&::pw_thread_loop_start) thread_loop_start{nullptr};
  decltype(&::pw_thread_loop_stop) thread_loop_stop{nullptr};
  decltype(&::pw_thread_loop_lock) thread_loop_lock{nullptr};
  decltype(&::pw_thread_loop_unlock) thread_loop_unlock{nullptr};
  decltype(&::pw_thread_loop_signal) thread_loop_signal{nullptr};
  decltype(&::pw_thread_loop_get_time) thread_loop_get_time{nullptr};
  decltype(&::pw_thread_loop_timed_wait_full) thread_loop_timed_wait_full{nullptr};
  decltype(&::pw_thread_loop_get_loop) thread_loop_get_loop{nullptr};
  decltype(&::pw_context_new) context_new{nullptr};
  decltype(&::pw_context_destroy) context_destroy{nullptr};
  decltype(&::pw_context_connect) context_connect{nullptr};
  decltype(&::pw_core_disconnect) core_disconnect{nullptr};
  decltype(&::pw_properties_new) properties_new{nullptr};
  decltype(&::pw_properties_set) properties_set{nullptr};
  decltype(&::pw_properties_free) properties_free{nullptr};
  decltype(&::pw_proxy_destroy) proxy_destroy{nullptr};
  decltype(&::pw_proxy_get_id) proxy_get_id{nullptr};
  decltype(&::pw_proxy_get_bound_id) proxy_get_bound_id{nullptr};
  decltype(&::pw_filter_new) filter_new{nullptr};
  decltype(&::pw_filter_destroy) filter_destroy{nullptr};
  decltype(&::pw_filter_add_listener) filter_add_listener{nullptr};
  decltype(&::pw_filter_connect) filter_connect{nullptr};
  decltype(&::pw_filter_get_node_id) filter_get_node_id{nullptr};
  decltype(&::pw_filter_add_port) filter_add_port{nullptr};
  decltype(&::pw_filter_get_dsp_buffer) filter_get_dsp_buffer{nullptr};

  bool loaded{false};

  // null without libpipewire, or with one that lacks a function used here
  static const PipeWire *get()
  {
    static const PipeWire pw{load()};
    return pw.loaded ? &pw : nullptr;
  }

 private:
  static PipeWire load()
  {
    PipeWire res;
    auto *lib{::dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL)};
    if (!lib)
    {
      LOGINFO("PipeWire is not available: {}", ::dlerror());
      return res;
    }

    // the first function missing, if one is
    std::string missing;
    auto bind = [lib, &missing](auto &fn, const char *name)
    {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(lib, name));
      if (!fn && missing.empty()) missing = name;
    };
    bind(res.init, "pw_init");
    bind(res.loop_new, "pw_loop_new");
    bind(res.loop_destroy, "pw_loop_destroy");
    bind(res.thread_loop_new, "pw_thread_loop_new");
    bind(res.thread_loop_destroy, "pw_thread_loop_destroy");
    bind(res.thread_loop_start, "pw_thread_loop_start");
    bind(res.thread_loop_stop, "pw_thread_loop_stop");
    bind(res.thread_loop_lock, "pw_thread_loop_lock");
    bind(res.thread_loop_unlock, "pw_thread_loop_unlock");
    bind(res.thread_loop_signal, "pw_thread_loop_signal");
    bind(res.thread_loop_get_time, "pw_thread_loop_get_time");
    bind(res.thread_loop_timed_wait_full, "pw_thread_loop_timed_wait_full");
    bind(res.thread_loop_get_loop, "pw_thread_loop_get_loop");
    bind(res.context_new, "pw_context_new");
    bind(res.context_destroy, "pw_context_destroy");
    bind(res.context_connect, "pw_context_connect");
    bind(res.core_disconnect, "pw_core_disconnect");
    bind(res.properties_new, "pw_properties_new");
    bind(res.properties_set, "pw_properties_set");
    bind(res.properties_free, "pw_properties_free");
    bind(res.proxy_destroy, "pw_proxy_destroy");
    bind(res.proxy_get_id, "pw_proxy_get_id");
    bind(res.proxy_get_bound_id, "pw_proxy_get_bound_id");
    bind(res.filter_new, "pw_filter_new");
    bind(res.filter_destroy, "pw_filter_destroy");
    bind(res.filter_add_listener, "pw_filter_add_listener");
    bind(res.filter_connect, "pw_filter_connect");
    bind(res.filter_get_node_id, "pw_filter_get_node_id");
    bind(res.filter_add_port, "pw_filter_add_port");
    bind(res.filter_get_dsp_buffer, "pw_filter_get_dsp_buffer");

    // the library stays loaded for the process's lifetime, as pw_init() wants
    res.loaded = missing.empty();
    if (!res.loaded) LOGINFO("[ERROR] libpipewire has no {}, so there is no PipeWire", missing);
    if (res.loaded) res.init(nullptr, nullptr);
    return res;
  }
};

std::string lookup(const spa_dict *props, const char *key)
{
  const char *value{props ? spa_dict_lookup(props, key) : nullptr};
  return value ? value : "";
}

uint32_t toUint(const std::string &text)
{
  return text.empty() ? 0 : static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
}

// every number in a list such as "[ 44100, 48000 ]"
std::vector<uint32_t> numbersIn(const std::string &text)
{
  std::vector<uint32_t> res;
  const char *p{text.c_str()};
  while (*p)
  {
    if (*p >= '0' && *p <= '9')
    {
      char *end{nullptr};
      res.push_back(static_cast<uint32_t>(std::strtoul(p, &end, 10)));
      p = end;
    }
    else
    {
      ++p;
    }
  }
  return res;
}

// the "name" of a metadata value such as { "name": "alsa_output.usb-..." }
std::string nameIn(const char *json)
{
  if (!json) return {};
  const std::string text{json};
  auto at{text.find("\"name\"")};
  if (at == std::string::npos) return {};
  at = text.find(':', at);
  if (at == std::string::npos) return {};
  at = text.find('"', at);
  if (at == std::string::npos) return {};
  std::string res;
  for (++at; at < text.size() && text[at] != '"'; ++at)
  {
    if (text[at] == '\\' && at + 1 < text.size()) ++at;
    res += text[at];
  }
  return res;
}

// a port's channel position for a menu: "Front Left" for FL. empty for one that names no
// position (AUX0, UNK), which channelName() then calls "Input 1"
std::string positionName(const std::string &channel)
{
  static const std::map<std::string, std::string> names{{"MONO", "Mono"},
                                                        {"FL", "Front Left"},
                                                        {"FR", "Front Right"},
                                                        {"FC", "Front Center"},
                                                        {"LFE", "LFE"},
                                                        {"SL", "Side Left"},
                                                        {"SR", "Side Right"},
                                                        {"FLC", "Front Left Center"},
                                                        {"FRC", "Front Right Center"},
                                                        {"RC", "Rear Center"},
                                                        {"RL", "Rear Left"},
                                                        {"RR", "Rear Right"},
                                                        {"TC", "Top Center"},
                                                        {"TFL", "Top Front Left"},
                                                        {"TFC", "Top Front Center"},
                                                        {"TFR", "Top Front Right"},
                                                        {"TRL", "Top Rear Left"},
                                                        {"TRC", "Top Rear Center"},
                                                        {"TRR", "Top Rear Right"}};
  const auto found{names.find(channel)};
  return found != names.end() ? found->second : std::string{};
}

class PipeWireBackend;

// what the registry says about the objects a device is made from. kept on the loop thread,
// read under the loop's lock
struct Node
{
  PipeWireBackend *backend{nullptr};
  uint32_t id{SPA_ID_INVALID};
  std::string name, description, mediaClass;
  uint32_t device{SPA_ID_INVALID};
  // node properties that say what kind of device it is, where the device object does not
  std::string api, bus, formFactor;
  bool pro{false};
  uint32_t channels{0};  // audio.channels, for a node whose ports are not known yet

  // the rate of the format it runs with now; 0 when it is suspended
  uint32_t rate{0};
  pw_proxy *proxy{nullptr};
  spa_hook listener{};
};

struct Port
{
  uint32_t node{SPA_ID_INVALID};
  bool output{false}, monitor{false};
  uint32_t index{0};
  std::string channel;
};

struct Device
{
  std::string api, bus, formFactor;
  // the card's description, which pipewire-pulse gives a sink or source of the card last,
  // and so is what RtAudio's pulse api saw
  std::string description;
};

struct Metadata
{
  PipeWireBackend *backend{nullptr};
  bool defaults{false};  // "default", or else "settings"
  pw_proxy *proxy{nullptr};
  spa_hook listener{};
};

// the filter's port handles, as pw_filter_add_port() gives them
using PortHandle = void *;

// the stream as the process callback sees it, from open() to stop()
struct Stream
{
  std::vector<PortHandle> inputs, outputs;
  std::vector<const float *> inputBlock;
  std::vector<float *> outputBlock;
  std::vector<float> silence;
  // what process() writes for an output port that has no buffer this cycle, unlinked
  std::vector<float> scratch;
  uint32_t maxFrames{0};

  // the rate of the cycles seen so far, and the most frames one had; set by the process
  // callback until open() has settled the rate the stream runs at
  std::atomic<uint32_t> seenRate{0}, seenFrames{0};
  std::atomic<uint32_t> rate{0};
  std::atomic<AudioStreamCallback *> callback{nullptr};
};

class PipeWireBackend final : public AudioBackend
{
 public:
  PipeWireBackend() : pw(PipeWire::get())
  {
    if (!pw) return;
    loop = pw->thread_loop_new("clap-wrapper-pipewire", nullptr);
    if (!loop) return;
    context = pw->context_new(pw->thread_loop_get_loop(loop), nullptr, 0);
    if (!context || pw->thread_loop_start(loop) != 0)
    {
      if (context) pw->context_destroy(context);
      pw->thread_loop_destroy(loop);
      context = nullptr;
      loop = nullptr;
      return;
    }

    {
      Lock lock(*this);
      notify = pw_loop_add_event(pw->thread_loop_get_loop(loop), &PipeWireBackend::notifyEvent, this);
      connect();
    }
    watcher = std::thread([this] { watch(); });
  }

  ~PipeWireBackend() override
  {
    if (!loop) return;
    stop();
    {
      std::lock_guard<std::mutex> guard(watchMutex);
      ending = true;
    }
    wake.notify_all();
    if (watcher.joinable()) watcher.join();
    {
      Lock lock(*this);
      disconnect();
      if (notify) pw_loop_destroy_source(pw->thread_loop_get_loop(loop), notify);
    }
    pw->thread_loop_stop(loop);
    pw->context_destroy(context);
    pw->thread_loop_destroy(loop);
  }

  std::string apiId() const override
  {
    return "pipewire";
  }
  std::string apiDisplayName() const override
  {
    return "PipeWire";
  }

  const std::vector<AudioDevice> &devices() override
  {
    if (!listed)
    {
      if (loop)
      {
        Lock lock(*this);
        if (!core || lost) connect();
        deviceList = list();
        signature = signatureOf(deviceList);
      }
      listed = true;
    }
    return deviceList;
  }

  void refreshDevices() override
  {
    listed = false;
  }

  // the default's node name, where that node has channels that way: wireplumber's default
  // source can be a sink's monitor, which is no input here
  std::string defaultInputDevice() override
  {
    return defaultDevice(true);
  }
  std::string defaultOutputDevice() override
  {
    return defaultDevice(false);
  }

  bool open(const AudioStreamRequest &request, AudioStreamInfo &granted, std::string &error) override
  {
    stop();
    granted = {};

    if (!pw)
    {
      error = "PipeWire is not installed: libpipewire-0.3.so.0 was not found.";
      return false;
    }
    if (!loop)
    {
      error = "PipeWire could not be started.";
      return false;
    }

    const bool hasOutput{!request.outputDevice.empty() && request.outputChannels > 0};
    const bool hasInput{!request.inputDevice.empty() && request.inputChannels > 0};
    if (!hasOutput && !hasInput)
    {
      error = "No audio device was chosen.";
      return false;
    }

    if (request.inputChannels > maxChannels || request.outputChannels > maxChannels)
    {
      error = "A stream has at most " + std::to_string(maxChannels) + " channels each way.";
      return false;
    }

    const auto &known{devices()};
    Lock lock(*this);
    if (!core || lost)
    {
      connect();
      if (!core)
      {
        error = "The PipeWire server is not running.";
        return false;
      }
    }

    // the devices' nodes and the ports the stream links to
    Target input, output;
    if ((hasInput && !resolve(known, request.inputDevice, true, request.inputFirstChannel,
                              request.inputChannels, input, error)) ||
        (hasOutput && !resolve(known, request.outputDevice, false, request.outputFirstChannel,
                               request.outputChannels, output, error)))
      return false;

    const auto clock{clockSettings()};
    uint32_t rate{request.sampleRate};
    if (rate == 0)
    {
      const auto *node{nodeById(hasOutput ? output.node : input.node)};
      rate = node && node->rate ? node->rate : clock.rate;
    }
    uint32_t frames{request.bufferFrames ? request.bufferFrames : clock.quantum};
    frames = std::clamp(frames, clock.minQuantum, std::max(clock.minQuantum, clock.maxQuantum));

    auto stream{std::make_unique<Stream>()};
    stream->maxFrames = frames;
    stream->silence.assign(frames, 0.f);
    stream->scratch.assign(frames, 0.f);
    current = std::move(stream);
    if (!makeFilter(*current, input, output, rate, frames, error))
    {
      abandon();
      return false;
    }

    // the filter's node and its ports in the registry, which links need
    const auto ports{static_cast<uint32_t>(input.ports.size() + output.ports.size())};
    waitFor(std::chrono::seconds(2), [this, ports] { return filterError || ownPorts() == ports; });
    if (filterError || ownPorts() != ports)
    {
      error = filterError ? "The audio stream could not be opened: " + filterMessage
                          : "The audio stream did not appear in PipeWire.";
      abandon();
      return false;
    }
    if (!link(input, true, error) || !link(output, false, error))
    {
      abandon();
      return false;
    }

    // watched from here on: a device that goes during the wait below fails the open
    streamInput = input;
    streamOutput = output;
    stopped = false;
    streamOpen = true;

    // the rate the graph runs the stream at: the one asked for where the graph could
    // switch to it, else the graph's. the first cycles say which
    {
      Unlock unlock(*this);
      const auto until{std::chrono::steady_clock::now() + std::chrono::seconds(1)};
      while (std::chrono::steady_clock::now() < until)
      {
        const auto seen{current->seenRate.load()};
        if (seen == rate) break;
        if (seen != 0 && std::chrono::steady_clock::now() + std::chrono::milliseconds(700) > until)
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    if (filterError || stopped)
    {
      error = filterError ? "The audio stream could not be opened: " + filterMessage
                          : "An audio device went away while the stream was opened.";
      abandon();
      return false;
    }
    const auto seen{current->seenRate.load()};
    const auto streamRate{seen ? seen : rate};
    current->rate = streamRate;

    granted.sampleRate = streamRate;
    granted.bufferFrames = current->seenFrames ? std::min(current->seenFrames.load(), frames) : frames;
    granted.maxFramesPerCallback = frames;
    granted.inputChannels = static_cast<uint32_t>(current->inputs.size());
    granted.outputChannels = static_cast<uint32_t>(current->outputs.size());
    // the latencies stay 0, unknown
    return true;
  }

  bool start(AudioStreamCallback &streamCallback, std::string &error) override
  {
    if (!current)
    {
      error = "No audio stream is open.";
      return false;
    }
    current->callback = &streamCallback;
    running = true;
    return true;
  }

  void stop() override
  {
    running = false;
    if (!current) return;

    // no process() call once this returns: the callback either sees no stream, or holds
    // it and is waited for
    active = nullptr;
    while (inProcess.load() > 0) std::this_thread::yield();

    {
      Lock lock(*this);
      unwatch();
      destroyFilter();
    }
    current.reset();
  }

  bool isOpen() const override
  {
    return current != nullptr;
  }

  // not once the stream has stopped on its own: no process() call is coming then
  bool isRunning() const override
  {
    return running && !stopped;
  }

 private:
  // the thread loop's lock, which everything the loop thread touches is under
  struct Lock
  {
    PipeWireBackend &backend;
    explicit Lock(PipeWireBackend &b) : backend(b)
    {
      backend.pw->thread_loop_lock(backend.loop);
    }
    ~Lock()
    {
      backend.pw->thread_loop_unlock(backend.loop);
    }
  };
  struct Unlock
  {
    PipeWireBackend &backend;
    explicit Unlock(PipeWireBackend &b) : backend(b)
    {
      backend.pw->thread_loop_unlock(backend.loop);
    }
    ~Unlock()
    {
      backend.pw->thread_loop_lock(backend.loop);
    }
  };

  // one side of a stream: the device's node and the global ids of the ports it links to
  struct Target
  {
    std::string name;
    uint32_t node{SPA_ID_INVALID};
    std::vector<uint32_t> ports;
  };

  struct Clock
  {
    uint32_t rate{48000}, quantum{1024}, minQuantum{32}, maxQuantum{2048};
    std::vector<uint32_t> allowedRates;
  };

  // [watcher-thread] while there is no server - it is not running yet, or it went away -
  // a connection is tried every two seconds; the host lists the devices again when one
  // succeeds. not while a stream is open: the host closes one that stopped first
  void watch()
  {
    std::unique_lock<std::mutex> guard(watchMutex);
    while (!ending)
    {
      wake.wait_for(guard, std::chrono::seconds(2));
      if (ending) break;
      guard.unlock();
      bool found{false};
      {
        Lock lock(*this);
        if ((!core || lost) && !streamOpen)
        {
          connect();
          found = core && !lost;
        }
      }
      if (found) post(AudioBackendEvent::DevicesChanged, {});
      guard.lock();
    }
  }

  // [locked] waits on the loop until done() or the time is up
  template <typename Done>
  void waitFor(std::chrono::milliseconds timeout, Done done)
  {
    timespec until{};
    pw->thread_loop_get_time(loop, &until, std::chrono::nanoseconds(timeout).count());
    while (!done())
    {
      if (pw->thread_loop_timed_wait_full(loop, &until) != 0) break;
    }
  }

  // [locked] a roundtrip: everything the server sent before it has arrived
  void sync()
  {
    if (!core) return;
    const auto seq{pw_core_sync(core, PW_ID_CORE, 0)};
    waitFor(std::chrono::seconds(2), [this, seq] { return lost || syncDone == seq; });
  }

  // [locked] the core, the registry and their listeners; the objects the registry lists
  // arrive by the time it returns
  void connect()
  {
    if (core) disconnect();
    lost = false;
    core = pw->context_connect(context, nullptr, 0);
    if (!core) return;

    static const pw_core_events coreEvents{PW_VERSION_CORE_EVENTS,
                                           &PipeWireBackend::coreInfo,
                                           &PipeWireBackend::coreDone,
                                           nullptr,
                                           &PipeWireBackend::coreError,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr,
                                           nullptr};
    pw_core_add_listener(core, &coreListener, &coreEvents, this);

    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    static const pw_registry_events registryEvents{
        PW_VERSION_REGISTRY_EVENTS, &PipeWireBackend::globalAdded, &PipeWireBackend::globalRemoved};
    pw_registry_add_listener(registry, &registryListener, &registryEvents, this);

    // the nodes' formats and the metadata come in answer to what the first sync brings
    sync();
    sync();
  }

  // [locked] the core and everything made on it
  void disconnect()
  {
    destroyFilter();
    if (core) pw->core_disconnect(core);  // destroys every proxy on it, ours included
    core = nullptr;
    registry = nullptr;
    nodes.clear();
    ports.clear();
    deviceObjects.clear();
    metadata.clear();
    defaultSink.clear();
    defaultSource.clear();
    settings.clear();
    coreDefaults.clear();
  }

  // [loop-thread]
  static void coreInfo(void *data, const pw_core_info *info)
  {
    auto *self{static_cast<PipeWireBackend *>(data)};
    if (!info || !info->props) return;
    for (const auto *key : {"default.clock.rate", "default.clock.allowed-rates", "default.clock.quantum",
                            "default.clock.min-quantum", "default.clock.max-quantum"})
    {
      const auto value{lookup(info->props, key)};
      if (!value.empty()) self->coreDefaults[key + std::strlen("default.")] = value;
    }
  }

  static void coreDone(void *data, uint32_t id, int seq)
  {
    auto *self{static_cast<PipeWireBackend *>(data)};
    if (id != PW_ID_CORE) return;
    self->syncDone = seq;
    self->pw->thread_loop_signal(self->loop, false);
  }

  static void coreError(void *data, uint32_t id, int, int res, const char *message)
  {
    auto *self{static_cast<PipeWireBackend *>(data)};
    if (id == PW_ID_CORE && res == -EPIPE)
    {
      // the server went away
      self->lost = true;
      self->streamStopped("The PipeWire server stopped.");
      self->post(AudioBackendEvent::DevicesChanged, {});
    }
    else if (std::find(self->linkIds.begin(), self->linkIds.end(), id) != self->linkIds.end())
    {
      // a link that could not be made
      LOGINFO("[ERROR] PipeWire: {} ({})", message ? message : "", res);
      self->filterError = true;
      self->filterMessage = message ? message : "";
    }
    else if (id == PW_ID_CORE)
    {
      LOGINFO("[ERROR] PipeWire: {} ({})", message ? message : "", res);
    }
    // else a node of the list that has no format to tell, being suspended
    self->pw->thread_loop_signal(self->loop, false);
  }

  static void globalAdded(void *data, uint32_t id, uint32_t, const char *type, uint32_t version,
                          const spa_dict *props)
  {
    auto *self{static_cast<PipeWireBackend *>(data)};
    if (!type) return;

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
    {
      const auto mediaClass{lookup(props, PW_KEY_MEDIA_CLASS)};
      if (mediaClass.rfind("Audio/Sink", 0) != 0 && mediaClass.rfind("Audio/Source", 0) != 0 &&
          mediaClass.rfind("Audio/Duplex", 0) != 0)
        return;

      auto node{std::make_unique<Node>()};
      node->backend = self;
      node->id = id;
      node->mediaClass = mediaClass;
      node->name = lookup(props, PW_KEY_NODE_NAME);
      node->description = lookup(props, PW_KEY_NODE_DESCRIPTION);
      if (node->description.empty()) node->description = lookup(props, PW_KEY_NODE_NICK);
      if (node->description.empty()) node->description = node->name;
      const auto device{lookup(props, PW_KEY_DEVICE_ID)};
      if (!device.empty()) node->device = toUint(device);
      node->api = lookup(props, PW_KEY_DEVICE_API);
      node->bus = lookup(props, PW_KEY_DEVICE_BUS);
      node->formFactor = lookup(props, PW_KEY_DEVICE_FORM_FACTOR);
      node->pro = lookup(props, "device.profile.pro") == "true";
      node->channels = toUint(lookup(props, PW_KEY_AUDIO_CHANNELS));
      if (node->name.empty()) return;

      // its format, for the rate it runs at
      node->proxy = static_cast<pw_proxy *>(
          pw_registry_bind(self->registry, id, type, std::min<uint32_t>(version, PW_VERSION_NODE), 0));
      if (node->proxy)
      {
        static const pw_node_events nodeEvents{PW_VERSION_NODE_EVENTS, nullptr,
                                               &PipeWireBackend::nodeParam};
        pw_node_add_listener(reinterpret_cast<pw_node *>(node->proxy), &node->listener, &nodeEvents,
                             node.get());
        uint32_t ids[]{SPA_PARAM_Format};
        pw_node_subscribe_params(reinterpret_cast<pw_node *>(node->proxy), ids, 1);
      }
      self->nodes[id] = std::move(node);
      self->devicesMayHaveChanged();
    }
    else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
    {
      Port port;
      port.node = toUint(lookup(props, PW_KEY_NODE_ID));
      port.output = lookup(props, PW_KEY_PORT_DIRECTION) == "out";
      port.monitor = lookup(props, PW_KEY_PORT_MONITOR) == "true";
      port.index = toUint(lookup(props, PW_KEY_PORT_ID));
      port.channel = lookup(props, PW_KEY_AUDIO_CHANNEL);
      if (lookup(props, PW_KEY_NODE_ID).empty()) return;
      self->ports[id] = port;
      if (port.node == self->filterNode) self->pw->thread_loop_signal(self->loop, false);
      if (self->nodes.count(port.node)) self->devicesMayHaveChanged();
    }
    else if (std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0)
    {
      if (lookup(props, PW_KEY_MEDIA_CLASS) != "Audio/Device") return;
      self->deviceObjects[id] = {lookup(props, PW_KEY_DEVICE_API), lookup(props, PW_KEY_DEVICE_BUS),
                                 lookup(props, PW_KEY_DEVICE_FORM_FACTOR),
                                 lookup(props, PW_KEY_DEVICE_DESCRIPTION)};
      self->devicesMayHaveChanged();
    }
    else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0)
    {
      const auto name{lookup(props, PW_KEY_METADATA_NAME)};
      if (name != "default" && name != "settings") return;
      auto meta{std::make_unique<Metadata>()};
      meta->backend = self;
      meta->defaults = name == "default";
      meta->proxy = static_cast<pw_proxy *>(pw_registry_bind(
          self->registry, id, type, std::min<uint32_t>(version, PW_VERSION_METADATA), 0));
      if (!meta->proxy) return;
      static const pw_metadata_events metadataEvents{PW_VERSION_METADATA_EVENTS,
                                                     &PipeWireBackend::metadataProperty};
      pw_metadata_add_listener(reinterpret_cast<pw_metadata *>(meta->proxy), &meta->listener,
                               &metadataEvents, meta.get());
      self->metadata[id] = std::move(meta);
    }
  }

  static void globalRemoved(void *data, uint32_t id)
  {
    auto *self{static_cast<PipeWireBackend *>(data)};

    if (auto node{self->nodes.find(id)}; node != self->nodes.end())
    {
      if (id == self->streamInput.node || id == self->streamOutput.node)
        self->streamStopped("The audio device " + node->second->description +
                            " is no longer available.");
      if (node->second->proxy) self->pw->proxy_destroy(node->second->proxy);
      self->nodes.erase(node);
      self->devicesMayHaveChanged();
    }
    if (auto port{self->ports.find(id)}; port != self->ports.end())
    {
      // one of the stream's own ports or the device ports it is linked to: a patchbay
      // took it, and the stream cannot run as it was opened
      const auto &in{self->streamInput.ports}, &out{self->streamOutput.ports};
      if (port->second.node == self->filterNode || std::find(in.begin(), in.end(), id) != in.end() ||
          std::find(out.begin(), out.end(), id) != out.end())
        self->streamStopped("A port of the audio stream was removed.");
      const auto ofDevice{self->nodes.count(port->second.node) > 0};
      self->ports.erase(port);
      if (ofDevice) self->devicesMayHaveChanged();
    }
    if (std::find(self->linkGlobals.begin(), self->linkGlobals.end(), id) != self->linkGlobals.end())
      self->streamStopped("A link of the audio stream was removed.");
    if (self->deviceObjects.erase(id)) self->devicesMayHaveChanged();
    if (auto meta{self->metadata.find(id)}; meta != self->metadata.end())
    {
      self->pw->proxy_destroy(meta->second->proxy);
      self->metadata.erase(meta);
    }
  }

  static void nodeParam(void *data, int, uint32_t id, uint32_t, uint32_t, const spa_pod *param)
  {
    auto *node{static_cast<Node *>(data)};
    if (id != SPA_PARAM_Format) return;

    uint32_t rate{0};
    uint32_t mediaType{0}, mediaSubtype{0};
    if (param && spa_format_parse(param, &mediaType, &mediaSubtype) >= 0 &&
        mediaType == SPA_MEDIA_TYPE_audio && mediaSubtype == SPA_MEDIA_SUBTYPE_raw)
    {
      spa_audio_info_raw info{};
      if (spa_format_audio_raw_parse(param, &info) >= 0) rate = info.rate;
    }
    if (rate == node->rate) return;
    node->rate = rate;
    node->backend->devicesMayHaveChanged();
  }

  static int metadataProperty(void *data, uint32_t subject, const char *key, const char *,
                              const char *value)
  {
    auto *meta{static_cast<Metadata *>(data)};
    auto *self{meta->backend};
    if (subject != PW_ID_CORE) return 0;

    if (meta->defaults)
    {
      if (!key)
      {
        self->defaultSink.clear();
        self->defaultSource.clear();
      }
      else if (std::strcmp(key, "default.audio.sink") == 0)
        self->defaultSink = nameIn(value);
      else if (std::strcmp(key, "default.audio.source") == 0)
        self->defaultSource = nameIn(value);
      else
        return 0;
    }
    else
    {
      if (!key)
        self->settings.clear();
      else if (value)
        self->settings[key] = value;
      else
        self->settings.erase(key);
    }
    self->devicesMayHaveChanged();
    return 0;
  }

  // [locked] the clock the settings set: the "settings" metadata's, else the core's
  // defaults, else PipeWire's own
  Clock clockSettings() const
  {
    auto value = [this](const std::string &key)
    {
      auto found{settings.find(key)};
      if (found != settings.end() && toUint(found->second) > 0) return found->second;
      auto fallback{coreDefaults.find(key)};
      return fallback != coreDefaults.end() ? fallback->second : std::string{};
    };
    Clock res;
    if (auto v{toUint(value("clock.rate"))}) res.rate = v;
    if (auto v{toUint(value("clock.quantum"))}) res.quantum = v;
    if (auto v{toUint(value("clock.min-quantum"))}) res.minQuantum = v;
    if (auto v{toUint(value("clock.max-quantum"))}) res.maxQuantum = v;
    auto allowed{settings.find("clock.allowed-rates")};
    res.allowedRates =
        numbersIn(allowed != settings.end() ? allowed->second : value("clock.allowed-rates"));
    return res;
  }

  std::string defaultDevice(bool input)
  {
    for (const auto &device : devices())
    {
      if ((input ? device.isDefaultInput : device.isDefaultOutput)) return device.id;
    }
    return {};
  }

  // [locked] the ports of a node in one direction, by port index. a sink's monitor outputs
  // are left out; a virtual source's outputs are all monitors, and are its channels
  std::vector<std::pair<uint32_t, const Port *>> portsOf(uint32_t node, bool output) const
  {
    const auto *n{nodeById(node)};
    const bool source{n && n->mediaClass.rfind("Audio/Source", 0) == 0};
    std::vector<std::pair<uint32_t, const Port *>> res;
    for (const auto &[id, port] : ports)
    {
      if (port.node == node && port.output == output && (!port.monitor || source))
        res.emplace_back(id, &port);
    }
    std::sort(res.begin(), res.end(),
              [](const auto &a, const auto &b) { return a.second->index < b.second->index; });
    return res;
  }

  // [locked]
  std::vector<AudioDevice> list() const
  {
    std::vector<AudioDevice> res;
    const auto clock{clockSettings()};

    std::vector<const Node *> sorted;
    for (const auto &[id, node] : nodes) sorted.push_back(node.get());
    std::sort(sorted.begin(), sorted.end(), [](const Node *a, const Node *b) { return a->id < b->id; });

    for (const auto *node : sorted)
    {
      AudioDevice device;
      device.id = node->name;
      device.name = node->description;
      device.legacyName = node->description;

      // a sink's input ports take what we output; a source's output ports give our input
      const bool sink{node->mediaClass.rfind("Audio/Sink", 0) == 0};
      const bool source{node->mediaClass.rfind("Audio/Source", 0) == 0};
      for (bool input : {true, false})
      {
        if ((input && sink) || (!input && source)) continue;
        auto &names{input ? device.inputChannelNames : device.outputChannelNames};
        for (const auto &[id, port] : portsOf(node->id, input))
          names.push_back(positionName(port->channel));
        auto count{static_cast<uint32_t>(names.size())};
        if (count == 0 && !(sink && source)) count = node->channels;
        (input ? device.inputChannels : device.outputChannels) = count;
      }
      if (device.inputChannels == 0 && device.outputChannels == 0) continue;

      Device traits{node->api, node->bus, node->formFactor, {}};
      if (auto object{deviceObjects.find(node->device)}; object != deviceObjects.end())
      {
        if (!object->second.description.empty()) device.legacyName = object->second.description;
        if (!object->second.api.empty()) traits.api = object->second.api;
        if (!object->second.bus.empty()) traits.bus = object->second.bus;
        if (!object->second.formFactor.empty()) traits.formFactor = object->second.formFactor;
      }
      device.isBluetooth = traits.api.find("bluez") != std::string::npos || traits.bus == "bluetooth";
      const auto &ff{traits.formFactor};
      device.isMicrophone =
          device.inputChannels > 0 &&
          (device.isBluetooth || ff == "microphone" || ff == "headset" || ff == "webcam" ||
           ff == "handset" || ff == "hands-free" || ff == "internal");
      device.isInterface = !device.isMicrophone && !device.isBluetooth &&
                           (node->pro || traits.bus == "usb" || traits.bus == "firewire");

      device.currentSampleRate = node->rate ? node->rate : clock.rate;
      if (!clock.allowedRates.empty())
        device.sampleRates = clock.allowedRates;
      else
        device.sampleRates = standardSampleRates();
      device.sampleRates.push_back(device.currentSampleRate);
      std::sort(device.sampleRates.begin(), device.sampleRates.end());
      device.sampleRates.erase(std::unique(device.sampleRates.begin(), device.sampleRates.end()),
                               device.sampleRates.end());
      // the graph switches to a rate a stream asks for only among the allowed ones, and a
      // bluetooth device's profile sets its own
      device.sampleRateSettable = !device.isBluetooth && device.sampleRates.size() > 1;
      if (!device.sampleRateSettable) device.sampleRates.assign(1, device.currentSampleRate);

      device.minBufferFrames = clock.minQuantum;
      device.maxBufferFrames = std::max(clock.minQuantum, clock.maxQuantum);
      device.isDefaultOutput = device.outputChannels > 0 && node->name == defaultSink;
      device.isDefaultInput = device.inputChannels > 0 && node->name == defaultSource;
      res.push_back(std::move(device));
    }
    return res;
  }

  static std::string signatureOf(const std::vector<AudioDevice> &list)
  {
    std::string res;
    for (const auto &d : list)
    {
      res += d.id + '\x1f' + d.name + '\x1f' + std::to_string(d.inputChannels) + '/' +
             std::to_string(d.outputChannels) + '@' + std::to_string(d.currentSampleRate) + ':';
      for (auto r : d.sampleRates) res += std::to_string(r) + ',';
      res += std::to_string(d.minBufferFrames) + '-' + std::to_string(d.maxBufferFrames);
      res += (d.isDefaultInput ? "i" : "") + std::string(d.isDefaultOutput ? "o" : "") + '\x1e';
    }
    return res;
  }

  // [loop-thread] only a list that reads differently now is news: our own filter's ports,
  // for one, are not
  void devicesMayHaveChanged()
  {
    const auto now{signatureOf(list())};
    if (now == signature) return;
    signature = now;
    post(AudioBackendEvent::DevicesChanged, {});
  }

  const Node *nodeById(uint32_t id) const
  {
    auto found{nodes.find(id)};
    return found != nodes.end() ? found->second.get() : nullptr;
  }

  // [locked] the node and ports a request names
  bool resolve(const std::vector<AudioDevice> &known, const std::string &reference, bool input,
               uint32_t first, uint32_t count, Target &target, std::string &error) const
  {
    const auto *device{findDevice(known, reference, input)};
    const auto name{device ? device->id : reference};
    const Node *node{nullptr};
    for (const auto &[id, n] : nodes)
    {
      if (n->name == name) node = n.get();
    }
    target.name = device ? device->name : reference;
    if (!node)
    {
      error = "The audio device " + target.name + " is not there.";
      return false;
    }
    target.node = node->id;
    const auto devicePorts{portsOf(node->id, input)};
    if (first + count > devicePorts.size())
    {
      error = "The audio device " + target.name + " has " + std::to_string(devicePorts.size()) + " " +
              (input ? "inputs" : "outputs") + ", not " + std::to_string(first + count) + ".";
      return false;
    }
    for (uint32_t c = 0; c < count; ++c) target.ports.push_back(devicePorts[first + c].first);
    return true;
  }

  // [locked] the filter with its ports, connected
  bool makeFilter(Stream &stream, const Target &input, const Target &output, uint32_t rate,
                  uint32_t frames, std::string &error)
  {
    const char *app{program_invocation_short_name};
    const std::string name{app && *app ? app : "clap-wrapper"};
    const auto latency{std::to_string(frames) + "/" + std::to_string(rate)};
    const auto category{!input.ports.empty() && !output.ports.empty() ? "Duplex"
                        : input.ports.empty()                         ? "Playback"
                                                                      : "Capture"};
    auto *props{pw->properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, category,
                                   PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME, name.c_str(),
                                   PW_KEY_NODE_LATENCY, latency.c_str(), PW_KEY_NODE_AUTOCONNECT,
                                   "false", nullptr)};
    pw->properties_set(props, PW_KEY_NODE_RATE, ("1/" + std::to_string(rate)).c_str());

    filterError = false;
    filterMessage.clear();
    filter = pw->filter_new(core, name.c_str(), props);
    if (!filter)
    {
      error = "The audio stream could not be made.";
      return false;
    }
    static const pw_filter_events filterEvents{PW_VERSION_FILTER_EVENTS,
                                               nullptr,
                                               &PipeWireBackend::filterState,
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               nullptr,
                                               &PipeWireBackend::processCallback,
                                               nullptr,
                                               nullptr};
    pw->filter_add_listener(filter, &filterListener, &filterEvents, this);

    for (bool in : {true, false})
    {
      const auto count{(in ? input : output).ports.size()};
      for (size_t c = 0; c < count; ++c)
      {
        const auto portName{(in ? "in_" : "out_") + std::to_string(c + 1)};
        auto *portProps{pw->properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                           PW_KEY_PORT_NAME, portName.c_str(), nullptr)};
        auto *port{pw->filter_add_port(filter, in ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                                       PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(uint32_t), portProps,
                                       nullptr, 0)};
        if (!port)
        {
          error = "The audio stream's ports could not be made.";
          return false;
        }
        (in ? stream.inputs : stream.outputs).push_back(port);
      }
    }

    // the process callback has the stream from its first cycle on
    stream.inputBlock.assign(stream.inputs.size(), nullptr);
    stream.outputBlock.assign(stream.outputs.size(), nullptr);
    active = &stream;

    if (pw->filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0)
    {
      error = "The audio stream could not be connected.";
      return false;
    }
    waitFor(std::chrono::seconds(2),
            [this] { return filterError || pw->filter_get_node_id(filter) != SPA_ID_INVALID; });
    filterNode = pw->filter_get_node_id(filter);
    if (filterNode == SPA_ID_INVALID)
    {
      error = filterError ? "The audio stream could not be connected: " + filterMessage
                          : "The audio stream could not be connected.";
      return false;
    }
    return true;
  }

  // [locked] the filter's ports in the registry
  uint32_t ownPorts() const
  {
    uint32_t res{0};
    for (const auto &[id, port] : ports) res += port.node == filterNode ? 1 : 0;
    return res;
  }

  // [locked] links between the filter's ports of one direction and the target's
  bool link(const Target &target, bool input, std::string &error)
  {
    if (target.ports.empty()) return true;
    const auto own{portsOf(filterNode, !input)};
    if (own.size() != target.ports.size())
    {
      error = "The audio stream's ports did not appear in PipeWire.";
      return false;
    }
    for (size_t c = 0; c < own.size(); ++c)
    {
      const auto ownPort{std::to_string(own[c].first)}, devicePort{std::to_string(target.ports[c])};
      const auto ownNode{std::to_string(filterNode)}, deviceNode{std::to_string(target.node)};
      auto *props{pw->properties_new(PW_KEY_LINK_OUTPUT_NODE, (input ? deviceNode : ownNode).c_str(),
                                     PW_KEY_LINK_OUTPUT_PORT, (input ? devicePort : ownPort).c_str(),
                                     PW_KEY_LINK_INPUT_NODE, (input ? ownNode : deviceNode).c_str(),
                                     PW_KEY_LINK_INPUT_PORT, (input ? ownPort : devicePort).c_str(),
                                     PW_KEY_OBJECT_LINGER, "false", nullptr)};
      auto *proxy{static_cast<pw_proxy *>(pw_core_create_object(
          core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0))};
      pw->properties_free(props);
      if (!proxy)
      {
        error = "The audio device " + target.name + " could not be linked to.";
        return false;
      }
      links.push_back(proxy);
      linkIds.push_back(pw->proxy_get_id(proxy));
    }
    sync();
    linkGlobals.clear();
    for (auto *proxy : links) linkGlobals.push_back(pw->proxy_get_bound_id(proxy));
    if (filterError)
    {
      error = "The audio device " + target.name + " could not be linked to: " + filterMessage;
      return false;
    }
    return true;
  }

  // [locked] a stream open() gives up on: no process() call once it returns
  void abandon()
  {
    active = nullptr;
    while (inProcess.load() > 0) std::this_thread::yield();
    unwatch();
    destroyFilter();
    current.reset();
  }

  // [locked] the stream is not watched any more, and an event it raised is not passed on
  void unwatch()
  {
    streamOpen = false;
    streamInput = {};
    streamOutput = {};
    pendingEvent = -1;
  }

  // [locked]
  void destroyFilter()
  {
    for (auto *proxy : links) pw->proxy_destroy(proxy);
    links.clear();
    linkIds.clear();
    linkGlobals.clear();
    if (filter) pw->filter_destroy(filter);
    filter = nullptr;
    filterNode = SPA_ID_INVALID;
  }

  // [loop-thread]
  static void filterState(void *data, pw_filter_state, pw_filter_state state, const char *message)
  {
    auto *self{static_cast<PipeWireBackend *>(data)};
    if (state == PW_FILTER_STATE_ERROR)
    {
      self->filterError = true;
      self->filterMessage = message ? message : "";
      self->streamStopped("The audio stream stopped: " + self->filterMessage);
    }
    self->pw->thread_loop_signal(self->loop, false);
  }

  // [audio-thread]
  static void processCallback(void *data, spa_io_position *position)
  {
    static_cast<PipeWireBackend *>(data)->process(position);
  }

  void process(spa_io_position *position)
  {
    ++inProcess;
    auto *stream{active.load()};
    if (!stream || !position)
    {
      --inProcess;
      return;
    }

    const auto frames{static_cast<uint32_t>(position->clock.duration)};
    const auto rate{position->clock.rate.denom};
    stream->seenRate = rate;
    if (frames > stream->seenFrames) stream->seenFrames = frames;

    // the graph switched to another rate under the stream: the plugin runs at the old one
    const auto streamRate{stream->rate.load()};
    if (streamRate != 0 && rate != streamRate && !stopped.exchange(true))
    {
      pendingRate = rate;
      pendingEvent = static_cast<int>(AudioBackendEvent::RateChanged);
      pw_loop_signal_event(pw->thread_loop_get_loop(loop), notify);
    }

    auto *callback{stopped ? nullptr : stream->callback.load()};
    const auto inputs{stream->inputs.size()}, outputs{stream->outputs.size()};
    for (size_t c = 0; c < outputs; ++c)
    {
      auto *buffer{static_cast<float *>(pw->filter_get_dsp_buffer(stream->outputs[c], frames))};
      stream->outputBlock[c] = buffer;
      if (buffer && !callback) std::memset(buffer, 0, frames * sizeof(float));
    }
    // every port's buffer, every cycle, used or not: an input port whose buffers are not
    // taken runs out of them, and has none from then on
    auto &in{stream->inputBlock};
    auto &out{stream->outputBlock};
    for (size_t c = 0; c < inputs; ++c)
      in[c] = static_cast<const float *>(pw->filter_get_dsp_buffer(stream->inputs[c], frames));

    if (callback)
    {
      // blocks of no more than the buffer the stream was opened with
      for (uint32_t done = 0; done < frames;)
      {
        const auto n{std::min(frames - done, stream->maxFrames)};
        const float *blockIn[maxChannels];
        float *blockOut[maxChannels];
        for (size_t c = 0; c < inputs && c < maxChannels; ++c)
          blockIn[c] = in[c] ? in[c] + done : stream->silence.data();
        for (size_t c = 0; c < outputs && c < maxChannels; ++c)
          blockOut[c] = out[c] ? out[c] + done : stream->scratch.data();
        callback->process(
            inputs ? blockIn : nullptr, static_cast<uint32_t>(std::min(inputs, maxChannels)),
            outputs ? blockOut : nullptr, static_cast<uint32_t>(std::min(outputs, maxChannels)), n);
        done += n;
      }
    }
    --inProcess;
  }

  // [loop-thread] an event the process callback raised, passed on from here
  static void notifyEvent(void *data, uint64_t)
  {
    auto *self{static_cast<PipeWireBackend *>(data)};
    const auto event{self->pendingEvent.exchange(-1)};
    if (event == static_cast<int>(AudioBackendEvent::RateChanged) && self->streamOpen)
      self->post(AudioBackendEvent::RateChanged,
                 "PipeWire was switched to " + std::to_string(self->pendingRate.load()) + " Hz");
  }

  // [loop-thread] the stream stopped on its own, once
  void streamStopped(const std::string &message)
  {
    if (!streamOpen || stopped.exchange(true)) return;
    post(AudioBackendEvent::StreamStopped, message);
  }

  void post(AudioBackendEvent event, const std::string &message)
  {
    if (onEvent) onEvent(event, message);
  }

  static constexpr size_t maxChannels{64};

  const PipeWire *pw{nullptr};
  pw_thread_loop *loop{nullptr};
  pw_context *context{nullptr};
  spa_source *notify{nullptr};

  // under the loop's lock from here on
  pw_core *core{nullptr};
  pw_registry *registry{nullptr};
  spa_hook coreListener{}, registryListener{};
  bool lost{false};
  int syncDone{-1};

  std::map<uint32_t, std::unique_ptr<Node>> nodes;
  std::map<uint32_t, Port> ports;
  std::map<uint32_t, Device> deviceObjects;
  std::map<uint32_t, std::unique_ptr<Metadata>> metadata;
  std::string defaultSink, defaultSource;
  std::map<std::string, std::string> settings, coreDefaults;
  std::string signature;

  pw_filter *filter{nullptr};
  spa_hook filterListener{};
  uint32_t filterNode{SPA_ID_INVALID};
  std::vector<pw_proxy *> links;
  std::vector<uint32_t> linkIds;
  // the links' global ids, which the registry removes them by
  std::vector<uint32_t> linkGlobals;
  bool filterError{false};
  std::string filterMessage;
  Target streamInput, streamOutput;
  bool streamOpen{false};

  std::mutex watchMutex;
  std::condition_variable wake;
  bool ending{false};
  std::thread watcher;

  // the main thread's
  bool listed{false};
  std::vector<AudioDevice> deviceList;

  std::unique_ptr<Stream> current;
  std::atomic<Stream *> active{nullptr};
  std::atomic<int> inProcess{0};
  std::atomic<bool> stopped{false};
  std::atomic<int> pendingEvent{-1};
  std::atomic<uint32_t> pendingRate{0};
  bool running{false};
};
}  // namespace

std::unique_ptr<AudioBackend> createPipeWireBackend()
{
  return std::make_unique<PipeWireBackend>();
}

bool pipeWireUsable()
{
  const auto *pw{PipeWire::get()};
  if (!pw) return false;
  auto *loop{pw->loop_new(nullptr)};
  if (!loop) return false;
  bool res{false};
  if (auto *context{pw->context_new(loop, nullptr, 0)})
  {
    // the connection is made, or refused, before this returns
    if (auto *core{pw->context_connect(context, nullptr, 0)})
    {
      res = true;
      pw->core_disconnect(core);
    }
    pw->context_destroy(context);
  }
  pw->loop_destroy(loop);
  return res;
}
}  // namespace freeaudio::clap_wrapper::standalone
