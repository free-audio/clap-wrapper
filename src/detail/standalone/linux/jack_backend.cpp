/*
 * The standalone's JACK backend, with libjack loaded at run time: a build has it whether or
 * not the machine it runs on has JACK, and without libjack or a running server it lists no
 * devices and says so when a stream is opened.
 *
 * A device is a client with physical or terminal audio ports, as RtAudio listed them: its
 * id is the client's name ("system"), its channels are the client's ports, and RtAudio's
 * name for it is the same name. The rate and the buffer are the server's, never set here.
 *
 * One client for the backend's lifetime, activated when it connects: it runs the process
 * callback, and hears of ports and clients coming and going. A stream is ports registered
 * on it and connected to the device's ports from the chosen channel on; stop() unregisters
 * them again.
 *
 * JACK's notifications only set flags: some JACK versions call them from the process
 * thread, and the shutdown one like a signal handler. A watcher thread looks at the flags
 * every 250 ms and raises the events, and while there is no server it tries for one every
 * two seconds, so that a server started or restarted later is found.
 *
 * Nothing in here throws.
 */

#include "detail/standalone/standalone_backends.h"
#include "detail/standalone/standalone_details.h"

#include <jack/jack.h>

#include <dlfcn.h>
#include <errno.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// libjack's functions, from the library dlopen()ed once per process. the declarations in
// jack.h give their types; nothing here links against them
struct Jack
{
  decltype(&::jack_client_open) client_open{nullptr};
  decltype(&::jack_client_close) client_close{nullptr};
  decltype(&::jack_get_client_name) get_client_name{nullptr};
  decltype(&::jack_activate) activate{nullptr};
  decltype(&::jack_deactivate) deactivate{nullptr};
  decltype(&::jack_set_process_callback) set_process_callback{nullptr};
  decltype(&::jack_on_info_shutdown) on_info_shutdown{nullptr};
  decltype(&::jack_set_sample_rate_callback) set_sample_rate_callback{nullptr};
  decltype(&::jack_set_buffer_size_callback) set_buffer_size_callback{nullptr};
  decltype(&::jack_set_client_registration_callback) set_client_registration_callback{nullptr};
  decltype(&::jack_set_port_registration_callback) set_port_registration_callback{nullptr};
  decltype(&::jack_get_sample_rate) get_sample_rate{nullptr};
  decltype(&::jack_get_buffer_size) get_buffer_size{nullptr};
  decltype(&::jack_get_ports) get_ports{nullptr};
  decltype(&::jack_free) free{nullptr};
  decltype(&::jack_port_by_name) port_by_name{nullptr};
  decltype(&::jack_port_by_id) port_by_id{nullptr};
  decltype(&::jack_port_flags) port_flags{nullptr};
  decltype(&::jack_port_type) port_type{nullptr};
  decltype(&::jack_port_name) port_name{nullptr};
  decltype(&::jack_port_short_name) port_short_name{nullptr};
  decltype(&::jack_port_register) port_register{nullptr};
  decltype(&::jack_port_unregister) port_unregister{nullptr};
  decltype(&::jack_port_get_buffer) port_get_buffer{nullptr};
  decltype(&::jack_connect) connect{nullptr};
  decltype(&::jack_port_get_latency_range) port_get_latency_range{nullptr};
  decltype(&::jack_set_error_function) set_error_function{nullptr};
  decltype(&::jack_set_info_function) set_info_function{nullptr};

  bool loaded{false};

  // null without libjack, or with one that lacks a function used here
  static const Jack *get()
  {
    static const Jack jack{load()};
    return jack.loaded ? &jack : nullptr;
  }

 private:
  static Jack load()
  {
    Jack res;
    auto *lib{::dlopen("libjack.so.0", RTLD_NOW | RTLD_LOCAL)};
    if (!lib)
    {
      LOGINFO("JACK is not available: {}", ::dlerror());
      return res;
    }

    // the first function missing, if one is
    std::string missing;
    auto bind = [lib, &missing](auto &fn, const char *name)
    {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(lib, name));
      if (!fn && missing.empty()) missing = name;
    };
    bind(res.client_open, "jack_client_open");
    bind(res.client_close, "jack_client_close");
    bind(res.get_client_name, "jack_get_client_name");
    bind(res.activate, "jack_activate");
    bind(res.deactivate, "jack_deactivate");
    bind(res.set_process_callback, "jack_set_process_callback");
    bind(res.on_info_shutdown, "jack_on_info_shutdown");
    bind(res.set_sample_rate_callback, "jack_set_sample_rate_callback");
    bind(res.set_buffer_size_callback, "jack_set_buffer_size_callback");
    bind(res.set_client_registration_callback, "jack_set_client_registration_callback");
    bind(res.set_port_registration_callback, "jack_set_port_registration_callback");
    bind(res.get_sample_rate, "jack_get_sample_rate");
    bind(res.get_buffer_size, "jack_get_buffer_size");
    bind(res.get_ports, "jack_get_ports");
    bind(res.free, "jack_free");
    bind(res.port_by_name, "jack_port_by_name");
    bind(res.port_by_id, "jack_port_by_id");
    bind(res.port_flags, "jack_port_flags");
    bind(res.port_type, "jack_port_type");
    bind(res.port_name, "jack_port_name");
    bind(res.port_short_name, "jack_port_short_name");
    bind(res.port_register, "jack_port_register");
    bind(res.port_unregister, "jack_port_unregister");
    bind(res.port_get_buffer, "jack_port_get_buffer");
    bind(res.connect, "jack_connect");
    bind(res.port_get_latency_range, "jack_port_get_latency_range");
    bind(res.set_error_function, "jack_set_error_function");
    bind(res.set_info_function, "jack_set_info_function");

    // the library stays loaded for the process's lifetime: its threads may outlive a client
    res.loaded = missing.empty();
    if (!res.loaded) LOGINFO("[ERROR] libjack has no {}, so there is no JACK", missing);
    if (res.loaded)
    {
      // libjack prints every failed connection to stderr; a missing server is reported
      // through the backend
      res.set_error_function([](const char *) {});
      res.set_info_function([](const char *) {});
    }
    return res;
  }
};

// the ports a jack_get_ports() call names, freed
std::vector<std::string> portNames(const Jack &jack, jack_client_t *client, const char *pattern,
                                   unsigned long flags)
{
  std::vector<std::string> res;
  if (auto **ports{jack.get_ports(client, pattern, JACK_DEFAULT_AUDIO_TYPE, flags)})
  {
    for (auto **p = ports; *p; ++p) res.emplace_back(*p);
    jack.free(ports);
  }
  return res;
}

std::string clientOf(const std::string &portName)
{
  return portName.substr(0, portName.find(':'));
}

// a regular expression that matches the ports of this client and no other
std::string clientPattern(const std::string &client)
{
  std::string res{"^"};
  for (auto c : client)
  {
    if (std::strchr("\\^$.|?*+()[]{}", c)) res += '\\';
    res += c;
  }
  return res + ":";
}

// a device's ports in one direction, as a stream connects to them: playback ports take
// what we output, capture ports give what we take in
std::vector<std::string> devicePorts(const Jack &jack, jack_client_t *client, const std::string &device,
                                     bool input)
{
  return portNames(jack, client, clientPattern(device).c_str(),
                   input ? JackPortIsOutput : JackPortIsInput);
}

// the stream as the process callback sees it, from open() to stop()
struct Stream
{
  std::vector<jack_port_t *> inputs, outputs;
  std::vector<const float *> inputBlock;
  std::vector<float *> outputBlock;
  uint32_t maxFrames{0};
  std::atomic<AudioStreamCallback *> callback{nullptr};
};

// the largest buffer a stream opens with: a server that reports more, or none, is not used
constexpr uint32_t maxBufferFrames{8192};

class JackBackend final : public AudioBackend
{
 public:
  JackBackend() : jack(Jack::get())
  {
    if (jack) watcher = std::thread([this] { watch(); });
  }

  ~JackBackend() override
  {
    stop();
    {
      std::lock_guard<std::mutex> guard(watchMutex);
      ending = true;
    }
    wake.notify_all();
    if (watcher.joinable()) watcher.join();
    std::lock_guard<std::mutex> guard(clientLock);
    disconnect();
  }

  std::string apiId() const override
  {
    return "jack";
  }
  std::string apiDisplayName() const override
  {
    return "JACK";
  }

  const std::vector<AudioDevice> &devices() override
  {
    if (!listed)
    {
      listed = true;
      list();
    }
    return deviceList;
  }

  void refreshDevices() override
  {
    listed = false;
  }

  std::string defaultInputDevice() override
  {
    return defaultDevice(devices(), true);
  }
  std::string defaultOutputDevice() override
  {
    return defaultDevice(devices(), false);
  }

  bool open(const AudioStreamRequest &request, AudioStreamInfo &granted, std::string &error) override
  {
    stop();
    granted = {};

    if (!jack)
    {
      error = "JACK is not installed: libjack.so.0 was not found.";
      return false;
    }

    const bool hasOutput{!request.outputDevice.empty() && request.outputChannels > 0};
    const bool hasInput{!request.inputDevice.empty() && request.inputChannels > 0};
    if (!hasOutput && !hasInput)
    {
      error = "No audio device was chosen.";
      return false;
    }

    const auto &known{devices()};
    std::lock_guard<std::mutex> guard(clientLock);
    if (!connect())
    {
      error = "The JACK server is not running.";
      return false;
    }

    const auto rate{jack->get_sample_rate(client)};
    const auto buffer{jack->get_buffer_size(client)};
    if (buffer == 0 || buffer > maxBufferFrames)
    {
      error = "The JACK server's buffer of " + std::to_string(buffer) + " frames cannot be used.";
      return false;
    }

    auto stream{std::make_unique<Stream>()};
    std::string inputDevice, outputDevice;
    std::vector<std::string> inputTargets, outputTargets;
    if ((hasInput && !resolve(known, request.inputDevice, true, request.inputFirstChannel,
                              request.inputChannels, inputDevice, inputTargets, error)) ||
        (hasOutput && !resolve(known, request.outputDevice, false, request.outputFirstChannel,
                               request.outputChannels, outputDevice, outputTargets, error)))
      return false;

    // our ports, and their connections to the device's
    for (size_t c = 0; c < inputTargets.size(); ++c)
    {
      auto *port{jack->port_register(client, ("in_" + std::to_string(c + 1)).c_str(),
                                     JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0)};
      if (!port) break;
      stream->inputs.push_back(port);
    }
    for (size_t c = 0; c < outputTargets.size(); ++c)
    {
      auto *port{jack->port_register(client, ("out_" + std::to_string(c + 1)).c_str(),
                                     JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0)};
      if (!port) break;
      stream->outputs.push_back(port);
    }
    if (stream->inputs.size() != inputTargets.size() || stream->outputs.size() != outputTargets.size())
    {
      error = "The JACK ports for the audio stream could not be registered.";
      unregister(*stream);
      return false;
    }
    for (size_t c = 0; c < inputTargets.size(); ++c)
    {
      const auto res{jack->connect(client, inputTargets[c].c_str(), jack->port_name(stream->inputs[c]))};
      if (res != 0 && res != EEXIST)
      {
        error = "Unable to connect to " + inputTargets[c] + ".";
        unregister(*stream);
        return false;
      }
    }
    for (size_t c = 0; c < outputTargets.size(); ++c)
    {
      const auto res{
          jack->connect(client, jack->port_name(stream->outputs[c]), outputTargets[c].c_str())};
      if (res != 0 && res != EEXIST)
      {
        error = "Unable to connect to " + outputTargets[c] + ".";
        unregister(*stream);
        return false;
      }
    }

    stream->maxFrames = buffer;
    stream->inputBlock.assign(stream->inputs.size(), nullptr);
    stream->outputBlock.assign(stream->outputs.size(), nullptr);

    granted.sampleRate = rate;
    granted.bufferFrames = buffer;
    granted.maxFramesPerCallback = buffer;
    granted.inputChannels = static_cast<uint32_t>(stream->inputs.size());
    granted.outputChannels = static_cast<uint32_t>(stream->outputs.size());
    granted.inputLatency = latencyOf(inputTargets, JackCaptureLatency, buffer);
    granted.outputLatency = latencyOf(outputTargets, JackPlaybackLatency, buffer);

    // what the watcher checks the running stream against
    streamTargets = inputTargets;
    streamTargets.insert(streamTargets.end(), outputTargets.begin(), outputTargets.end());
    streamRate = rate;
    streamOpen = true;
    stopped = false;
    current = std::move(stream);
    active = current.get();
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

    std::lock_guard<std::mutex> guard(clientLock);
    streamOpen = false;
    streamTargets.clear();
    if (client && !dead) unregister(*current);
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
  // [locked] the backend's client, connected and activated; false without a server
  bool connect()
  {
    if (client && dead)
    {
      // the server shut down: the stream's ports went with the client, and a new one is
      // tried. the dead one is let go, not closed: jack2's jack_client_close() crashes on a
      // client whose server has gone
      if (current)
      {
        active = nullptr;
        while (inProcess.load() > 0) std::this_thread::yield();
        current->inputs.clear();
        current->outputs.clear();
      }
      client = nullptr;
    }
    if (client) return true;

    jack_status_t status{};
    const char *name{program_invocation_short_name};
    client = jack->client_open(name && *name ? name : "clap-wrapper", JackNoStartServer, &status);
    if (!client)
    {
      found = false;  // the watcher looks for a server from now on
      return false;
    }
    dead = false;
    deadReported = false;
    ownName = jack->get_client_name(client);
    lastRate = rate = jack->get_sample_rate(client);
    lastBuffer = buffer = jack->get_buffer_size(client);

    jack->set_process_callback(client, &JackBackend::processCallback, this);
    jack->on_info_shutdown(client, &JackBackend::shutdownCallback, this);
    jack->set_sample_rate_callback(client, &JackBackend::sampleRateCallback, this);
    jack->set_buffer_size_callback(client, &JackBackend::bufferSizeCallback, this);
    jack->set_client_registration_callback(client, &JackBackend::clientCallback, this);
    jack->set_port_registration_callback(client, &JackBackend::portCallback, this);
    if (jack->activate(client) != 0)
    {
      jack->client_close(client);
      client = nullptr;
      found = false;
      return false;
    }
    return true;
  }

  // [locked]
  void disconnect()
  {
    if (!client) return;
    if (!dead)
    {
      jack->deactivate(client);
      jack->client_close(client);
    }
    client = nullptr;
  }

  // [locked] the clients with physical or terminal ports, in the order the server lists
  // them
  std::vector<std::string> deviceClients()
  {
    std::vector<std::string> res;
    for (auto flags : {JackPortIsPhysical, JackPortIsTerminal})
    {
      for (const auto &port : portNames(*jack, client, nullptr, flags))
      {
        const auto name{clientOf(port)};
        if (name != ownName && std::find(res.begin(), res.end(), name) == res.end()) res.push_back(name);
      }
    }
    return res;
  }

  // [locked] the devices and their ports, to tell a change by
  std::string graphSignature()
  {
    std::string res;
    for (const auto &name : deviceClients())
    {
      res += name + '\x1e';
      for (bool input : {true, false})
      {
        for (const auto &port : devicePorts(*jack, client, name, input)) res += port + '\x1f';
        res += '\x1d';
      }
    }
    return res;
  }

  // [main-thread]
  void list()
  {
    deviceList.clear();
    if (!jack) return;
    std::lock_guard<std::mutex> guard(clientLock);
    if (!connect()) return;

    const auto rate{jack->get_sample_rate(client)};
    const auto buffer{jack->get_buffer_size(client)};
    for (const auto &name : deviceClients())
    {
      AudioDevice device;
      device.id = device.name = device.legacyName = name;
      for (bool input : {true, false})
      {
        auto &names{input ? device.inputChannelNames : device.outputChannelNames};
        for (const auto &port : devicePorts(*jack, client, name, input))
        {
          auto *p{jack->port_by_name(client, port.c_str())};
          names.emplace_back(p ? jack->port_short_name(p) : port.substr(port.find(':') + 1));
        }
        (input ? device.inputChannels : device.outputChannels) = static_cast<uint32_t>(names.size());
      }
      if (device.inputChannels == 0 && device.outputChannels == 0) continue;

      device.sampleRates.assign(1, rate);
      device.currentSampleRate = rate;
      device.sampleRateSettable = false;
      device.minBufferFrames = device.maxBufferFrames = buffer;
      deviceList.push_back(std::move(device));
    }

    const auto defaultIn{defaultDevice(deviceList, true)}, defaultOut{defaultDevice(deviceList, false)};
    for (auto &device : deviceList)
    {
      device.isDefaultInput = device.id == defaultIn;
      device.isDefaultOutput = device.id == defaultOut;
    }
    listedSignature = graphSignature();
  }

  // jack has no default device: "system" where it has channels that way, else the first
  // device that has, as RtAudio took the first
  static std::string defaultDevice(const std::vector<AudioDevice> &list, bool input)
  {
    const AudioDevice *first{nullptr};
    for (const auto &device : list)
    {
      if ((input ? device.inputChannels : device.outputChannels) == 0) continue;
      if (device.id == "system") return device.id;
      if (!first) first = &device;
    }
    return first ? first->id : std::string{};
  }

  // [locked] the ports of the device a request names, from its first channel
  bool resolve(const std::vector<AudioDevice> &known, const std::string &reference, bool input,
               uint32_t first, uint32_t count, std::string &device, std::vector<std::string> &targets,
               std::string &error)
  {
    const auto *listedDevice{findDevice(known, reference, input)};
    device = listedDevice ? listedDevice->id : reference;
    const auto ports{devicePorts(*jack, client, device, input)};
    if (ports.empty())
    {
      error = "The audio device " + device + " is not there.";
      return false;
    }
    if (first + count > ports.size())
    {
      error = "The audio device " + device + " has " + std::to_string(ports.size()) + " " +
              (input ? "inputs" : "outputs") + ", not " + std::to_string(first + count) + ".";
      return false;
    }
    targets.assign(ports.begin() + first, ports.begin() + first + count);
    return true;
  }

  // [locked] the most any of these ports has, in frames, the buffer left out
  uint32_t latencyOf(const std::vector<std::string> &ports, jack_latency_callback_mode_t mode,
                     uint32_t buffer) const
  {
    uint32_t most{0};
    for (const auto &name : ports)
    {
      auto *port{jack->port_by_name(client, name.c_str())};
      if (!port) continue;
      jack_latency_range_t range{};
      jack->port_get_latency_range(port, mode, &range);
      most = std::max(most, static_cast<uint32_t>(range.max));
    }
    return most > buffer ? most - buffer : 0;
  }

  // [locked]
  void unregister(Stream &stream)
  {
    for (auto *port : stream.inputs) jack->port_unregister(client, port);
    for (auto *port : stream.outputs) jack->port_unregister(client, port);
    stream.inputs.clear();
    stream.outputs.clear();
  }

  // [audio-thread]
  static int processCallback(jack_nframes_t frames, void *arg)
  {
    static_cast<JackBackend *>(arg)->process(frames);
    return 0;
  }

  void process(uint32_t frames)
  {
    ++inProcess;
    auto *stream{active.load()};
    if (!stream || stream->maxFrames == 0)
    {
      --inProcess;
      return;
    }

    const auto inputs{stream->inputs.size()}, outputs{stream->outputs.size()};
    auto *callback{stopped ? nullptr : stream->callback.load()};
    for (size_t c = 0; c < outputs; ++c)
    {
      auto *buffer{static_cast<float *>(jack->port_get_buffer(stream->outputs[c], frames))};
      stream->outputBlock[c] = buffer;
      if (!callback) std::memset(buffer, 0, frames * sizeof(float));
    }
    if (callback)
    {
      std::vector<const float *> &in{stream->inputBlock};
      std::vector<float *> &out{stream->outputBlock};
      for (size_t c = 0; c < inputs; ++c)
        in[c] = static_cast<const float *>(jack->port_get_buffer(stream->inputs[c], frames));

      // blocks of no more than the buffer the stream was opened with, which another
      // client may have made larger since
      for (uint32_t done = 0; done < frames;)
      {
        const auto n{std::min(frames - done, stream->maxFrames)};
        callback->process(inputs ? in.data() : nullptr, static_cast<uint32_t>(inputs),
                          outputs ? out.data() : nullptr, static_cast<uint32_t>(outputs), n);
        done += n;
        if (done < frames)
        {
          for (auto &p : in) p += n;
          for (auto &p : out) p += n;
        }
      }
    }
    --inProcess;
  }

  // [notification-thread] flags only: the watcher acts on them
  static void shutdownCallback(jack_status_t, const char *reason, void *arg)
  {
    auto *self{static_cast<JackBackend *>(arg)};
    std::strncpy(self->shutdownReason, reason ? reason : "", sizeof(self->shutdownReason) - 1);
    self->dead = true;
  }

  static int sampleRateCallback(jack_nframes_t rate, void *arg)
  {
    static_cast<JackBackend *>(arg)->rate = rate;
    return 0;
  }

  static int bufferSizeCallback(jack_nframes_t frames, void *arg)
  {
    static_cast<JackBackend *>(arg)->buffer = frames;
    return 0;
  }

  static void clientCallback(const char *, int, void *arg)
  {
    static_cast<JackBackend *>(arg)->graphChanged = true;
  }

  static void portCallback(jack_port_id_t, int, void *arg)
  {
    static_cast<JackBackend *>(arg)->graphChanged = true;
  }

  // [watcher-thread]
  void watch()
  {
    auto nextProbe{std::chrono::steady_clock::now()};
    std::unique_lock<std::mutex> lock(watchMutex);
    while (!ending)
    {
      wake.wait_for(lock, std::chrono::milliseconds(250));
      if (ending) break;
      lock.unlock();
      check(nextProbe);
      lock.lock();
    }
  }

  // [watcher-thread] what the notifications flagged, as events
  void check(std::chrono::steady_clock::time_point &nextProbe)
  {
    std::vector<std::pair<AudioBackendEvent, std::string>> events;
    {
      std::lock_guard<std::mutex> guard(clientLock);
      if (client && dead && !deadReported)
      {
        deadReported = true;
        found = false;
        const std::string reason{shutdownReason};
        if (streamOpen && !stopped.exchange(true))
          events.emplace_back(AudioBackendEvent::StreamStopped,
                              "The JACK server shut down" + (reason.empty() ? "" : ": " + reason) + ".");
        events.emplace_back(AudioBackendEvent::DevicesChanged, std::string{});
      }

      if (!client || dead)
      {
        // a server to connect to, once there is one: the host lists the devices again
        const auto now{std::chrono::steady_clock::now()};
        if (!found && now >= nextProbe)
        {
          nextProbe = now + std::chrono::seconds(2);
          jack_status_t status{};
          if (auto *probe{jack->client_open("clap-wrapper-probe", JackNoStartServer, &status)})
          {
            jack->client_close(probe);
            found = true;
            events.emplace_back(AudioBackendEvent::DevicesChanged, std::string{});
          }
        }
      }
      else
      {
        if (const auto now{rate.load()}; now != lastRate)
        {
          lastRate = now;
          if (streamOpen && now != streamRate && !stopped.exchange(true))
            events.emplace_back(AudioBackendEvent::RateChanged,
                                "The JACK server was switched to " + std::to_string(now) + " Hz");
          events.emplace_back(AudioBackendEvent::DevicesChanged, std::string{});
        }
        if (const auto now{buffer.load()}; now != lastBuffer)
        {
          // a larger buffer is split into blocks of the stream's
          lastBuffer = now;
          events.emplace_back(AudioBackendEvent::DevicesChanged, std::string{});
        }
        if (graphChanged.exchange(false))
        {
          // our own ports come and go too, and are no device
          if (auto now{graphSignature()}; now != listedSignature)
          {
            listedSignature = now;
            events.emplace_back(AudioBackendEvent::DevicesChanged, std::string{});
          }
          for (const auto &port : streamTargets)
          {
            if (!streamOpen || jack->port_by_name(client, port.c_str())) continue;
            if (!stopped.exchange(true))
              events.emplace_back(AudioBackendEvent::StreamStopped,
                                  "The audio device " + clientOf(port) + " is no longer available.");
            break;
          }
        }
      }
    }
    for (const auto &[event, message] : events)
    {
      if (onEvent) onEvent(event, message);
    }
  }

  const Jack *jack{nullptr};

  // the client and everything the watcher shares with the main thread are under it
  std::mutex clientLock;
  jack_client_t *client{nullptr};
  std::string ownName;
  std::string listedSignature;
  std::vector<std::string> streamTargets;
  uint32_t streamRate{0};
  bool streamOpen{false};
  bool deadReported{false};
  bool found{true};
  uint32_t lastRate{0}, lastBuffer{0};

  // set by the notifications
  std::atomic<bool> dead{false}, graphChanged{false};
  std::atomic<uint32_t> rate{0}, buffer{0};
  char shutdownReason[256]{};

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
  bool running{false};
};
}  // namespace

std::unique_ptr<AudioBackend> createJackBackend()
{
  return std::make_unique<JackBackend>();
}

bool jackUsable()
{
  const auto *jack{Jack::get()};
  if (!jack) return false;
  jack_status_t status{};
  auto *probe{jack->client_open("clap-wrapper-probe", JackNoStartServer, &status)};
  if (!probe) return false;
  jack->client_close(probe);
  return true;
}
}  // namespace freeaudio::clap_wrapper::standalone
