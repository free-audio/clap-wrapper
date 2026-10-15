/*
 * The standalone's MIDI on Linux: the ALSA sequencer, which PipeWire bridges, JACK setups
 * reach through a2jmidid, and every Linux synth and DAW speaks. libasound is loaded at run
 * time: without it, or without a sequencer (no /dev/snd/seq), there is no MIDI and the
 * standalone runs all the same.
 *
 * One sequencer client for the backend's lifetime, named after the standalone. Its input
 * port is subscribed to the sources chosen, its output port to the destinations chosen,
 * and a virtual input or output is a port of its own under the name given, for other
 * applications to connect to. None of our ports is ever listed.
 *
 * Client and port numbers move - USB clients are numbered in plug order - so a port's id
 * is "<client name>:<port name>", with "#2" and on for a repeat in its direction, as WinMM
 * ids have their ordinal. Its legacyName is RtMidi's "<client>:<port> <c>:<p>".
 *
 * Our input and output ports are private: only our own subscriptions reach them, so that
 * openInputs() decides alone what comes in. A subscription is made again when a port of
 * that id comes back, since a replugged device can take another client:port.
 *
 * One thread reads, started on the first call from the main thread, once the host has its
 * handlers in place: it polls the sequencer and a pipe that wakes it to end, decodes each
 * event with its status byte, one message at a time, and puts sysex that arrives in
 * pieces back together per source. The sender thread writes to each destination in turn,
 * sysex in chunks, paced to the MIDI cable's rate for a hardware port, and gives up on a
 * destination that takes nothing for a while rather than holding the sender.
 *
 * Nothing in here throws.
 */

#include "detail/standalone/standalone_backends.h"
#include "detail/standalone/standalone_details.h"

#include <alsa/asoundlib.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// libasound's functions, from the library dlopen()ed once per process. the declarations in
// asoundlib.h give their types; nothing here links against them. the snd_seq_ev_set_*
// helpers are macros on the event and need nothing
struct Alsa
{
  decltype(&::snd_seq_open) seq_open{nullptr};
  decltype(&::snd_seq_close) seq_close{nullptr};
  decltype(&::snd_seq_set_client_name) seq_set_client_name{nullptr};
  decltype(&::snd_seq_client_id) seq_client_id{nullptr};
  decltype(&::snd_seq_create_simple_port) seq_create_simple_port{nullptr};
  decltype(&::snd_seq_delete_simple_port) seq_delete_simple_port{nullptr};
  decltype(&::snd_seq_connect_from) seq_connect_from{nullptr};
  decltype(&::snd_seq_disconnect_from) seq_disconnect_from{nullptr};
  decltype(&::snd_seq_connect_to) seq_connect_to{nullptr};
  decltype(&::snd_seq_disconnect_to) seq_disconnect_to{nullptr};
  decltype(&::snd_seq_client_info_malloc) seq_client_info_malloc{nullptr};
  decltype(&::snd_seq_client_info_free) seq_client_info_free{nullptr};
  decltype(&::snd_seq_client_info_set_client) seq_client_info_set_client{nullptr};
  decltype(&::snd_seq_client_info_get_client) seq_client_info_get_client{nullptr};
  decltype(&::snd_seq_client_info_get_name) seq_client_info_get_name{nullptr};
  decltype(&::snd_seq_client_info_get_type) seq_client_info_get_type{nullptr};
  decltype(&::snd_seq_query_next_client) seq_query_next_client{nullptr};
  decltype(&::snd_seq_port_info_malloc) seq_port_info_malloc{nullptr};
  decltype(&::snd_seq_port_info_free) seq_port_info_free{nullptr};
  decltype(&::snd_seq_port_info_set_client) seq_port_info_set_client{nullptr};
  decltype(&::snd_seq_port_info_set_port) seq_port_info_set_port{nullptr};
  decltype(&::snd_seq_port_info_get_client) seq_port_info_get_client{nullptr};
  decltype(&::snd_seq_port_info_get_port) seq_port_info_get_port{nullptr};
  decltype(&::snd_seq_port_info_get_name) seq_port_info_get_name{nullptr};
  decltype(&::snd_seq_port_info_get_capability) seq_port_info_get_capability{nullptr};
  decltype(&::snd_seq_port_info_get_type) seq_port_info_get_type{nullptr};
  decltype(&::snd_seq_query_next_port) seq_query_next_port{nullptr};
  decltype(&::snd_seq_poll_descriptors_count) seq_poll_descriptors_count{nullptr};
  decltype(&::snd_seq_poll_descriptors) seq_poll_descriptors{nullptr};
  decltype(&::snd_seq_event_input) seq_event_input{nullptr};
  decltype(&::snd_seq_event_output_direct) seq_event_output_direct{nullptr};
  decltype(&::snd_midi_event_new) midi_event_new{nullptr};
  decltype(&::snd_midi_event_free) midi_event_free{nullptr};
  decltype(&::snd_midi_event_no_status) midi_event_no_status{nullptr};
  decltype(&::snd_midi_event_reset_encode) midi_event_reset_encode{nullptr};
  decltype(&::snd_midi_event_encode) midi_event_encode{nullptr};
  decltype(&::snd_midi_event_decode) midi_event_decode{nullptr};
  decltype(&::snd_lib_error_set_handler) lib_error_set_handler{nullptr};

  bool loaded{false};

  // null without libasound, or with one that lacks a function used here
  static const Alsa *get()
  {
    static const Alsa alsa{load()};
    return alsa.loaded ? &alsa : nullptr;
  }

 private:
  static Alsa load()
  {
    Alsa res;
    auto *lib{::dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL)};
    if (!lib)
    {
      LOGINFO("ALSA is not available, so there is no MIDI: {}", ::dlerror());
      return res;
    }

    // the first function missing, if one is
    std::string missing;
    auto bind = [lib, &missing](auto &fn, const char *name)
    {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(lib, name));
      if (!fn && missing.empty()) missing = name;
    };
    bind(res.seq_open, "snd_seq_open");
    bind(res.seq_close, "snd_seq_close");
    bind(res.seq_set_client_name, "snd_seq_set_client_name");
    bind(res.seq_client_id, "snd_seq_client_id");
    bind(res.seq_create_simple_port, "snd_seq_create_simple_port");
    bind(res.seq_delete_simple_port, "snd_seq_delete_simple_port");
    bind(res.seq_connect_from, "snd_seq_connect_from");
    bind(res.seq_disconnect_from, "snd_seq_disconnect_from");
    bind(res.seq_connect_to, "snd_seq_connect_to");
    bind(res.seq_disconnect_to, "snd_seq_disconnect_to");
    bind(res.seq_client_info_malloc, "snd_seq_client_info_malloc");
    bind(res.seq_client_info_free, "snd_seq_client_info_free");
    bind(res.seq_client_info_set_client, "snd_seq_client_info_set_client");
    bind(res.seq_client_info_get_client, "snd_seq_client_info_get_client");
    bind(res.seq_client_info_get_name, "snd_seq_client_info_get_name");
    bind(res.seq_client_info_get_type, "snd_seq_client_info_get_type");
    bind(res.seq_query_next_client, "snd_seq_query_next_client");
    bind(res.seq_port_info_malloc, "snd_seq_port_info_malloc");
    bind(res.seq_port_info_free, "snd_seq_port_info_free");
    bind(res.seq_port_info_set_client, "snd_seq_port_info_set_client");
    bind(res.seq_port_info_set_port, "snd_seq_port_info_set_port");
    bind(res.seq_port_info_get_client, "snd_seq_port_info_get_client");
    bind(res.seq_port_info_get_port, "snd_seq_port_info_get_port");
    bind(res.seq_port_info_get_name, "snd_seq_port_info_get_name");
    bind(res.seq_port_info_get_capability, "snd_seq_port_info_get_capability");
    bind(res.seq_port_info_get_type, "snd_seq_port_info_get_type");
    bind(res.seq_query_next_port, "snd_seq_query_next_port");
    bind(res.seq_poll_descriptors_count, "snd_seq_poll_descriptors_count");
    bind(res.seq_poll_descriptors, "snd_seq_poll_descriptors");
    bind(res.seq_event_input, "snd_seq_event_input");
    bind(res.seq_event_output_direct, "snd_seq_event_output_direct");
    bind(res.midi_event_new, "snd_midi_event_new");
    bind(res.midi_event_free, "snd_midi_event_free");
    bind(res.midi_event_no_status, "snd_midi_event_no_status");
    bind(res.midi_event_reset_encode, "snd_midi_event_reset_encode");
    bind(res.midi_event_encode, "snd_midi_event_encode");
    bind(res.midi_event_decode, "snd_midi_event_decode");
    bind(res.lib_error_set_handler, "snd_lib_error_set_handler");

    // the library stays loaded for the process's lifetime
    res.loaded = missing.empty();
    if (!res.loaded) LOGINFO("[ERROR] libasound has no {}, so there is no MIDI", missing);
    return res;
  }
};

// libasound's errors, which it prints to stderr by default: a machine without a sequencer
// is told of once, by the backend
void quiet(const char *, int, const char *, int, const char *, ...)
{
}

// a port as the listing found it
struct ListedPort
{
  MidiPort port;
  snd_seq_addr_t address{};
  bool kernel{false};  // a kernel client's: hardware, behind a rawmidi buffer
};

// a subscription of ours
struct Connection
{
  snd_seq_addr_t address{};
  bool kernel{false};
};

// the sysex chunks a send() is cut into
constexpr size_t sysexChunk{256};

// a hardware port takes sysex at the cable's rate: 31250 baud, ten bits a byte. faster,
// its rawmidi buffer fills, and the kernel drops what does not fit
constexpr double cableBytesPerSecond{3125.0};

// how long one send() waits, in all, for destinations that take nothing
constexpr auto sendPatience{std::chrono::milliseconds(500)};

// the most a sysex being put back together may grow to; one larger is dropped
constexpr size_t maxSysex{1 << 20};

class AlsaSeqMidiBackend final : public MidiBackend
{
 public:
  AlsaSeqMidiBackend() : alsa(Alsa::get())
  {
    if (!alsa) return;

    // libasound prints a missing sequencer to stderr; it is logged once here instead
    alsa->lib_error_set_handler(&quiet);
    if (alsa->seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0)
    {
      LOGINFO("[WARNING] There is no ALSA sequencer, so there is no MIDI");
      seq = nullptr;
      return;
    }
    const char *app{program_invocation_short_name};
    alsa->seq_set_client_name(seq, app && *app ? app : "clap-wrapper");
    ownClient = alsa->seq_client_id(seq);

    // private: our own subscriptions need no SUBS capability
    constexpr unsigned int type{SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION};
    inputPort = alsa->seq_create_simple_port(seq, "Input",
                                             SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT, type);
    outputPort = alsa->seq_create_simple_port(seq, "Output",
                                              SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_NO_EXPORT, type);
    // the system's announcements of clients and ports coming and going
    announcePort = alsa->seq_create_simple_port(seq, "Announcements",
                                                SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT,
                                                SND_SEQ_PORT_TYPE_APPLICATION);
    if (announcePort >= 0)
      alsa->seq_connect_from(seq, announcePort, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);

    if (alsa->midi_event_new(16, &decoder) < 0) decoder = nullptr;
    if (alsa->midi_event_new(16, &encoder) < 0) encoder = nullptr;
    if (decoder) alsa->midi_event_no_status(decoder, 1);
    if (encoder) alsa->midi_event_no_status(encoder, 1);
  }

  ~AlsaSeqMidiBackend() override
  {
    if (reader.joinable())
    {
      const char byte{0};
      [[maybe_unused]] auto written{::write(wake[1], &byte, 1)};
      reader.join();
    }
    for (auto fd : wake)
    {
      if (fd >= 0) ::close(fd);
    }
    if (decoder) alsa->midi_event_free(decoder);
    if (encoder) alsa->midi_event_free(encoder);
    if (seq) alsa->seq_close(seq);  // its ports and subscriptions go with it
  }

  std::string apiId() const override
  {
    return "alsa";
  }

  std::vector<MidiPort> inputs() override
  {
    startReading();
    std::vector<MidiPort> res;
    for (const auto &listed : list(true)) res.push_back(listed.port);
    return res;
  }

  std::vector<MidiPort> outputs() override
  {
    startReading();
    std::vector<MidiPort> res;
    for (const auto &listed : list(false)) res.push_back(listed.port);
    return res;
  }

  std::vector<std::string> openInputs(const std::vector<std::string> &ids) override
  {
    startReading();
    {
      std::lock_guard<std::mutex> guard(lock);
      wantedInputs = ids;
    }
    return subscribe(true);
  }

  std::vector<std::string> openOutputs(const std::vector<std::string> &ids) override
  {
    startReading();
    {
      std::lock_guard<std::mutex> guard(lock);
      wantedOutputs = ids;
    }
    return subscribe(false);
  }

  bool supportsVirtualPorts() const override
  {
    return seq != nullptr;
  }

  bool setVirtualInput(const std::string &name) override
  {
    startReading();
    return setVirtual(virtualInput, name, SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
  }

  bool setVirtualOutput(const std::string &name) override
  {
    startReading();
    return setVirtual(virtualOutput, name, SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
  }

  void send(const uint8_t *data, size_t size) override
  {
    if (!seq || !data || size == 0) return;
    std::vector<Connection> destinations;
    int virtualPort{-1};
    {
      std::lock_guard<std::mutex> guard(lock);
      for (const auto &[id, connection] : connectedOutputs) destinations.push_back(connection);
      virtualPort = virtualOutput;
    }

    auto deadline{std::chrono::steady_clock::now() + sendPatience};
    for (const auto &destination : destinations) sendTo(outputPort, &destination, data, size, deadline);
    // other applications' subscriptions to our virtual output: one try each, as one
    // that does not keep up must not hold up the rest
    if (virtualPort >= 0) sendTo(virtualPort, nullptr, data, size, deadline);
  }

 private:
  // [main-thread] the reader, once the host's handlers are in place
  void startReading()
  {
    if (!seq || reader.joinable() || readerFailed) return;
    if (::pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0)
    {
      readerFailed = true;
      return;
    }
    reader = std::thread([this] { read(); });
  }

  // the ports other clients have in one direction: sources for input, destinations for
  // output
  std::vector<ListedPort> list(bool input)
  {
    std::vector<ListedPort> res;
    if (!seq) return res;

    const auto wanted{
        static_cast<unsigned int>(input ? SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ
                                        : SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE)};
    snd_seq_client_info_t *client{nullptr};
    snd_seq_port_info_t *port{nullptr};
    alsa->seq_client_info_malloc(&client);
    alsa->seq_port_info_malloc(&port);
    if (!client || !port)
    {
      if (client) alsa->seq_client_info_free(client);
      if (port) alsa->seq_port_info_free(port);
      return res;
    }

    std::map<std::string, int> seen;
    std::map<std::string, int> names;
    {
      std::lock_guard<std::mutex> guard(lock);
      alsa->seq_client_info_set_client(client, -1);
      while (alsa->seq_query_next_client(seq, client) >= 0)
      {
        const auto clientId{alsa->seq_client_info_get_client(client)};
        if (clientId == SND_SEQ_CLIENT_SYSTEM || clientId == ownClient) continue;
        const std::string clientName{alsa->seq_client_info_get_name(client)};
        const bool kernel{alsa->seq_client_info_get_type(client) == SND_SEQ_KERNEL_CLIENT};

        alsa->seq_port_info_set_client(port, clientId);
        alsa->seq_port_info_set_port(port, -1);
        while (alsa->seq_query_next_port(seq, port) >= 0)
        {
          const auto caps{alsa->seq_port_info_get_capability(port)};
          if ((caps & wanted) != wanted || (caps & SND_SEQ_PORT_CAP_NO_EXPORT)) continue;

          ListedPort listed;
          listed.address.client = static_cast<unsigned char>(clientId);
          listed.address.port = static_cast<unsigned char>(alsa->seq_port_info_get_port(port));
          listed.kernel = kernel;
          const std::string portName{alsa->seq_port_info_get_name(port)};

          const auto base{clientName + ":" + portName};
          const auto ordinal{++seen[base]};
          listed.port.id = ordinal == 1 ? base : base + "#" + std::to_string(ordinal);

          // a port that names its client says enough by itself, as most do
          auto name{portName.compare(0, clientName.size(), clientName) == 0
                        ? portName
                        : clientName + ": " + portName};
          const auto repeat{++names[name]};
          listed.port.name = repeat == 1 ? name : name + " (" + std::to_string(repeat) + ")";

          // rtmidi's name, for the ports rtmidi listed: midi, synth and application ones
          const auto type{alsa->seq_port_info_get_type(port)};
          if (type &
              (SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTH | SND_SEQ_PORT_TYPE_APPLICATION))
            listed.port.legacyName = base + " " + std::to_string(clientId) + ":" +
                                     std::to_string(static_cast<int>(listed.address.port));
          res.push_back(std::move(listed));
        }
      }
    }

    alsa->seq_client_info_free(client);
    alsa->seq_port_info_free(port);
    return res;
  }

  // our port of one direction subscribed to exactly the wanted ports that are there now.
  // what was subscribed to a port that went, or now is another client:port, is let go;
  // the rest is subscribed again, which the kernel answers with -EBUSY where it still is
  std::vector<std::string> subscribe(bool input)
  {
    std::vector<std::string> opened;
    if (!seq) return opened;

    const auto available{list(input)};
    auto &connected{input ? connectedInputs : connectedOutputs};
    const auto own{input ? inputPort : outputPort};

    std::lock_guard<std::mutex> guard(lock);
    const auto &wanted{input ? wantedInputs : wantedOutputs};
    auto find = [&available](const std::string &id)
    {
      return std::find_if(available.begin(), available.end(),
                          [&id](const ListedPort &l) { return l.port.id == id; });
    };

    for (auto it = connected.begin(); it != connected.end();)
    {
      const auto now{find(it->first)};
      if (std::find(wanted.begin(), wanted.end(), it->first) != wanted.end() && now != available.end() &&
          now->address.client == it->second.address.client &&
          now->address.port == it->second.address.port)
      {
        ++it;
        continue;
      }
      if (input)
        alsa->seq_disconnect_from(seq, own, it->second.address.client, it->second.address.port);
      else
        alsa->seq_disconnect_to(seq, own, it->second.address.client, it->second.address.port);
      it = connected.erase(it);
    }

    for (const auto &id : wanted)
    {
      const auto found{find(id)};
      if (found == available.end()) continue;
      const auto res{input ? alsa->seq_connect_from(seq, own, found->address.client, found->address.port)
                           : alsa->seq_connect_to(seq, own, found->address.client, found->address.port)};
      if (res < 0 && res != -EBUSY)
      {
        LOGINFO("[ERROR] Unable to connect MIDI {} '{}': {}", input ? "input" : "output", id, res);
        continue;
      }
      connected[id] = {found->address, found->kernel};
      opened.push_back(id);
    }
    return opened;
  }

  bool setVirtual(int &port, const std::string &name, unsigned int caps)
  {
    if (!seq) return name.empty();
    std::lock_guard<std::mutex> guard(lock);
    if (port >= 0) alsa->seq_delete_simple_port(seq, port);
    port = -1;
    if (name.empty()) return true;

    port = alsa->seq_create_simple_port(seq, name.c_str(), caps,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (port < 0)
    {
      LOGINFO("[ERROR] Unable to create the virtual MIDI port '{}': {}", name, port);
      port = -1;
      return false;
    }
    return true;
  }

  // [sender-thread] one event from our port to one destination, or to the port's
  // subscribers without one, written now. a destination that is full is waited for until
  // the send()'s deadline; the subscribers are tried once
  bool output(int port, const Connection *destination, snd_seq_event_t &event,
              std::chrono::steady_clock::time_point deadline)
  {
    snd_seq_ev_set_source(&event, port);
    if (destination)
      snd_seq_ev_set_dest(&event, destination->address.client, destination->address.port);
    else
      snd_seq_ev_set_subs(&event);
    snd_seq_ev_set_direct(&event);
    while (true)
    {
      int res{0};
      {
        std::lock_guard<std::mutex> guard(lock);
        res = alsa->seq_event_output_direct(seq, &event);
      }
      if (res >= 0) return true;
      const bool full{res == -EAGAIN || res == -ENOMEM};
      if (!full || !destination || std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }

  // [sender-thread] one message to one destination, or to the port's subscribers
  void sendTo(int port, const Connection *destination, const uint8_t *data, size_t size,
              std::chrono::steady_clock::time_point &deadline)
  {
    if (data[0] != 0xF0)
    {
      if (!encoder) return;
      snd_seq_event_t event;
      snd_seq_ev_clear(&event);
      alsa->midi_event_reset_encode(encoder);
      const auto used{alsa->midi_event_encode(encoder, data, static_cast<long>(size), &event)};
      if (used <= 0 || event.type == SND_SEQ_EVENT_NONE) return;
      if (!output(port, destination, event, deadline))
        LOGINFO("[WARNING] A MIDI destination is not taking messages; one was dropped");
      return;
    }

    // sysex as variable events of a chunk each, which the receiver puts back together
    const bool paced{destination && destination->kernel};
    for (size_t done = 0; done < size;)
    {
      const auto n{std::min(sysexChunk, size - done)};
      snd_seq_event_t event;
      snd_seq_ev_clear(&event);
      snd_seq_ev_set_sysex(&event, static_cast<unsigned int>(n), const_cast<uint8_t *>(data + done));
      if (!output(port, destination, event, deadline))
      {
        // the rest is dropped; a message begun is ended, so that the receiver does not take
        // what comes next as part of it
        if (done > 0)
        {
          uint8_t end{0xF7};
          snd_seq_ev_clear(&event);
          snd_seq_ev_set_sysex(&event, 1, &end);
          output(port, destination, event, deadline);
        }
        LOGINFO("[WARNING] A MIDI destination is not taking sysex; a message was cut short");
        return;
      }
      done += n;
      if (paced)
      {
        // the time the cable takes for it, which does not count against the patience
        const std::chrono::duration<double> cable{static_cast<double>(n) / cableBytesPerSecond};
        std::this_thread::sleep_for(cable);
        deadline += std::chrono::duration_cast<std::chrono::steady_clock::duration>(cable);
      }
    }
  }

  // [reader-thread]
  void read()
  {
    std::vector<pollfd> fds;
    {
      std::lock_guard<std::mutex> guard(lock);
      const auto count{alsa->seq_poll_descriptors_count(seq, POLLIN)};
      fds.resize(static_cast<size_t>(std::max(count, 0)) + 1);
      alsa->seq_poll_descriptors(seq, fds.data() + 1, static_cast<unsigned int>(count), POLLIN);
    }
    fds[0] = {wake[0], POLLIN, 0};

    while (true)
    {
      if (::poll(fds.data(), fds.size(), -1) < 0)
      {
        if (errno == EINTR) continue;
        break;
      }
      if (fds[0].revents) break;

      while (true)
      {
        snd_seq_event_t *event{nullptr};
        int res{0};
        {
          std::lock_guard<std::mutex> guard(lock);
          res = alsa->seq_event_input(seq, &event);
        }
        if (res == -ENOSPC)
        {
          // the kernel's queue for us overflowed and was emptied: what was being put
          // together has a gap now
          partial.clear();
          continue;
        }
        if (res == -EAGAIN || res < 0 || !event) break;
        handle(*event);
      }
    }
  }

  // [reader-thread] one event: an announcement, a piece of sysex, or a short message
  void handle(const snd_seq_event_t &event)
  {
    if (event.dest.port == announcePort)
    {
      switch (event.type)
      {
        case SND_SEQ_EVENT_CLIENT_START:
        case SND_SEQ_EVENT_CLIENT_EXIT:
        case SND_SEQ_EVENT_CLIENT_CHANGE:
        case SND_SEQ_EVENT_PORT_START:
        case SND_SEQ_EVENT_PORT_EXIT:
        case SND_SEQ_EVENT_PORT_CHANGE:
          if (event.data.addr.client == ownClient) break;
          // a port chosen before may be back, at another client:port
          subscribe(true);
          subscribe(false);
          if (onPortsChanged) onPortsChanged();
          break;
        default:
          break;
      }
      return;
    }

    if (event.type == SND_SEQ_EVENT_SYSEX)
    {
      sysex(event);
      return;
    }

    if (!decoder) return;
    // one event can decode to several messages: a 14 bit controller is two, an (N)RPN four
    unsigned char bytes[16];
    const auto size{alsa->midi_event_decode(decoder, bytes, sizeof(bytes), &event)};
    for (long at = 0; at < size;)
    {
      const auto n{static_cast<long>(shortMidiMessageSize(bytes[at]))};
      if (n == 0 || at + n > size) break;
      if (onMessage) onMessage(bytes + at, static_cast<size_t>(n));
      at += n;
    }
  }

  // [reader-thread] a piece of sysex, byte by byte per source: a message from F0 to its F7
  // to onMessage whole, realtime bytes within it as messages of their own, and what a
  // status byte breaks off dropped
  void sysex(const snd_seq_event_t &event)
  {
    const auto *data{static_cast<const uint8_t *>(event.data.ext.ptr)};
    const auto size{static_cast<size_t>(event.data.ext.len)};
    if (!data || size == 0) return;

    const auto source{static_cast<uint32_t>(event.source.client) << 8 | event.source.port};
    auto &state{partial[source]};
    for (size_t i = 0; i < size; ++i)
    {
      const auto byte{data[i]};
      if (byte >= 0xF8)
      {
        if (onMessage) onMessage(&byte, 1);
      }
      else if (byte == 0xF0)
      {
        state.bytes.assign(1, byte);
        state.open = true;
      }
      else if (byte == 0xF7)
      {
        if (!state.open) continue;
        state.bytes.push_back(byte);
        if (onMessage) onMessage(state.bytes.data(), state.bytes.size());
        finish(state);
      }
      else if (byte & 0x80)
      {
        finish(state);
      }
      else if (state.open)
      {
        if (state.bytes.size() >= maxSysex)
        {
          LOGINFO("[WARNING] A MIDI sysex message larger than {} bytes was dropped", maxSysex);
          finish(state);
          continue;
        }
        state.bytes.push_back(byte);
      }
    }
  }

  // a sysex being put back together
  struct Partial
  {
    std::vector<uint8_t> bytes;
    bool open{false};
  };

  // done with a sysex: a large one's memory is not kept
  static void finish(Partial &state)
  {
    state.open = false;
    state.bytes.clear();
    if (state.bytes.capacity() > 65536) state.bytes.shrink_to_fit();
  }

  const Alsa *alsa{nullptr};
  snd_seq_t *seq{nullptr};
  int ownClient{-1};
  int inputPort{-1}, outputPort{-1}, announcePort{-1};
  int virtualInput{-1}, virtualOutput{-1};
  snd_midi_event_t *decoder{nullptr}, *encoder{nullptr};

  // every sequencer call is made under it: the handle is shared by the main, reader and
  // sender threads. the subscriptions are under it too
  std::mutex lock;
  std::vector<std::string> wantedInputs, wantedOutputs;
  std::map<std::string, Connection> connectedInputs, connectedOutputs;

  int wake[2]{-1, -1};
  bool readerFailed{false};
  std::thread reader;
  std::map<uint32_t, Partial> partial;
};
}  // namespace

std::unique_ptr<MidiBackend> createAlsaSeqMidiBackend()
{
  return std::make_unique<AlsaSeqMidiBackend>();
}
}  // namespace freeaudio::clap_wrapper::standalone
