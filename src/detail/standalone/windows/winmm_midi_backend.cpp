/*
 * The standalone's WinMM MIDI backend.
 *
 * WinMM knows a port by its place in a list that changes whenever a device comes or goes,
 * and by a name that two of the same device share. A port's id is therefore its name and
 * its place among the ports of that name ("Midex 8 Port 1#2"), which survives a device
 * coming or going elsewhere; its name repeats as "Name (2)". RtMidi called it by its name
 * and its place in the whole list ("Name 3"), which is the legacy name.
 *
 * An input delivers short messages whole (MIM_DATA) and sysex through buffers we hand it,
 * in pieces of a buffer's size (MIM_LONGDATA), which are put back together here. WinMM has
 * no virtual ports and says nothing when a device comes or goes.
 *
 * Nothing in here throws.
 */

#include "detail/standalone/standalone_backends.h"

#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// sysex buffers per input, and their size: a sysex longer than one is reassembled
constexpr size_t sysexBuffers{4};
constexpr size_t sysexBufferSize{4096};

// a sysex longer than this is dropped: a sender that never sends F7 does not take the
// memory with it
constexpr size_t maxSysexSize{1 << 20};

std::string toUtf8(const wchar_t *text)
{
  if (!text || !*text) return {};
  const auto length{::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr)};
  if (length <= 1) return {};
  std::string res(static_cast<size_t>(length - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, res.data(), length, nullptr, nullptr);
  return res;
}

// the size of a short message by its status byte; 0 for a byte that does not start one
size_t shortMessageSize(uint8_t status)
{
  if (status < 0x80) return 0;
  if (status < 0xc0) return 3;
  if (status < 0xe0) return 2;
  if (status < 0xf0) return 3;
  switch (status)
  {
    case 0xf1:
    case 0xf3:
      return 2;
    case 0xf2:
      return 3;
    case 0xf0:
    case 0xf7:
      return 0;
    default:
      return 1;
  }
}

// a port as WinMM lists it, with its index in that list now
struct ListedPort
{
  MidiPort port;
  UINT index{0};
};

// the ports of one direction, named and numbered as the top of this file says
std::vector<ListedPort> listPorts(bool input)
{
  std::vector<ListedPort> res;
  std::map<std::string, int> seen;
  const auto count{input ? ::midiInGetNumDevs() : ::midiOutGetNumDevs()};
  for (UINT i = 0; i < count; ++i)
  {
    std::string name;
    if (input)
    {
      MIDIINCAPSW caps{};
      if (::midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
      name = toUtf8(caps.szPname);
    }
    else
    {
      MIDIOUTCAPSW caps{};
      if (::midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) continue;
      name = toUtf8(caps.szPname);
    }

    const auto ordinal{++seen[name]};
    ListedPort listed;
    listed.index = i;
    listed.port.id = name + "#" + std::to_string(ordinal);
    listed.port.name = ordinal == 1 ? name : name + " (" + std::to_string(ordinal) + ")";
    listed.port.legacyName = name + " " + std::to_string(i);
    res.push_back(std::move(listed));
  }
  return res;
}

std::vector<MidiPort> portsOf(const std::vector<ListedPort> &listed)
{
  std::vector<MidiPort> res;
  for (const auto &l : listed) res.push_back(l.port);
  return res;
}

// an open input, at a fixed address for WinMM's callback
struct InputPort
{
  std::string id;
  HMIDIIN handle{nullptr};
  std::array<MIDIHDR, sysexBuffers> headers{};
  std::array<std::array<char, sysexBufferSize>, sysexBuffers> buffers{};
  std::atomic<bool> closing{false};

  // [callback] the sysex being reassembled. WinMM calls one handle's callback from one
  // thread at a time
  std::vector<uint8_t> sysex;
  bool inSysex{false};

  const std::function<void(const uint8_t *, size_t)> *onMessage{nullptr};
};

// an open output
struct OutputPort
{
  std::string id;
  HMIDIOUT handle{nullptr};
  HANDLE done{nullptr};
};

class WinMmMidiBackend final : public MidiBackend
{
 public:
  ~WinMmMidiBackend() override
  {
    openInputs({});
    openOutputs({});
  }

  std::string apiId() const override
  {
    return "winmm";
  }

  std::vector<MidiPort> inputs() override
  {
    return portsOf(listPorts(true));
  }
  std::vector<MidiPort> outputs() override
  {
    return portsOf(listPorts(false));
  }

  std::vector<std::string> openInputs(const std::vector<std::string> &ids) override
  {
    // what is not wanted any more is closed first, so that a port that moved in the list
    // can be opened again by its id
    for (auto it = inputPorts.begin(); it != inputPorts.end();)
    {
      if (std::find(ids.begin(), ids.end(), (*it)->id) == ids.end())
      {
        closeInput(**it);
        it = inputPorts.erase(it);
      }
      else
        ++it;
    }

    const auto listed{listPorts(true)};
    std::vector<std::string> res;
    for (const auto &id : ids)
    {
      auto open = std::find_if(inputPorts.begin(), inputPorts.end(),
                               [&id](const auto &p) { return p->id == id; });
      if (open != inputPorts.end())
      {
        res.push_back(id);
        continue;
      }
      auto port =
          std::find_if(listed.begin(), listed.end(), [&id](const auto &l) { return l.port.id == id; });
      if (port == listed.end()) continue;

      auto input{std::make_unique<InputPort>()};
      input->id = id;
      input->onMessage = &onMessage;
      if (openInput(*input, port->index))
      {
        inputPorts.push_back(std::move(input));
        res.push_back(id);
      }
    }
    return res;
  }

  std::vector<std::string> openOutputs(const std::vector<std::string> &ids) override
  {
    std::lock_guard<std::mutex> lock(outputMutex);
    for (auto it = outputPorts.begin(); it != outputPorts.end();)
    {
      if (std::find(ids.begin(), ids.end(), it->id) == ids.end())
      {
        closeOutput(*it);
        it = outputPorts.erase(it);
      }
      else
        ++it;
    }

    const auto listed{listPorts(false)};
    std::vector<std::string> res;
    for (const auto &id : ids)
    {
      auto open = std::find_if(outputPorts.begin(), outputPorts.end(),
                               [&id](const auto &p) { return p.id == id; });
      if (open != outputPorts.end())
      {
        res.push_back(id);
        continue;
      }
      auto port =
          std::find_if(listed.begin(), listed.end(), [&id](const auto &l) { return l.port.id == id; });
      if (port == listed.end()) continue;

      OutputPort output;
      output.id = id;
      output.done = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
      if (output.done &&
          ::midiOutOpen(&output.handle, port->index, reinterpret_cast<DWORD_PTR>(output.done), 0,
                        CALLBACK_EVENT) == MMSYSERR_NOERROR)
      {
        outputPorts.push_back(output);
        res.push_back(id);
      }
      else if (output.done)
      {
        ::CloseHandle(output.done);
      }
    }
    return res;
  }

  bool supportsVirtualPorts() const override
  {
    return false;
  }
  bool setVirtualInput(const std::string &name) override
  {
    return name.empty();
  }
  bool setVirtualOutput(const std::string &name) override
  {
    return name.empty();
  }

  void send(const uint8_t *data, size_t size) override
  {
    if (!data || size == 0) return;

    std::lock_guard<std::mutex> lock(outputMutex);
    if (data[0] != 0xf0 && size <= 3)
    {
      DWORD message{0};
      for (size_t i = 0; i < size; ++i) message |= static_cast<DWORD>(data[i]) << (8 * i);
      for (auto &output : outputPorts) ::midiOutShortMsg(output.handle, message);
      return;
    }

    // sysex, from a buffer of our own that WinMM may read until it says it is done
    std::vector<char> buffer(data, data + size);
    for (auto &output : outputPorts)
    {
      MIDIHDR header{};
      header.lpData = buffer.data();
      header.dwBufferLength = static_cast<DWORD>(buffer.size());
      header.dwBytesRecorded = header.dwBufferLength;
      if (::midiOutPrepareHeader(output.handle, &header, sizeof(header)) != MMSYSERR_NOERROR) continue;
      if (::midiOutLongMsg(output.handle, &header, sizeof(header)) == MMSYSERR_NOERROR)
      {
        // at 3125 bytes a second over a cable, with a second to spare
        const auto limit{::GetTickCount64() + 1000 + size * 1000 / 3125};
        while (!(header.dwFlags & MHDR_DONE))
        {
          if (::GetTickCount64() > limit)
          {
            ::midiOutReset(output.handle);
            break;
          }
          ::WaitForSingleObject(output.done, 50);
        }
      }
      ::midiOutUnprepareHeader(output.handle, &header, sizeof(header));
    }
  }

 private:
  static void CALLBACK inputCallback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1,
                                     DWORD_PTR)
  {
    auto &input{*reinterpret_cast<InputPort *>(instance)};
    if (message == MIM_DATA)
    {
      const uint8_t bytes[3]{static_cast<uint8_t>(param1 & 0xff),
                             static_cast<uint8_t>((param1 >> 8) & 0xff),
                             static_cast<uint8_t>((param1 >> 16) & 0xff)};
      const auto size{shortMessageSize(bytes[0])};
      if (size && *input.onMessage) (*input.onMessage)(bytes, size);
      return;
    }

    if (message != MIM_LONGDATA && message != MIM_LONGERROR) return;

    auto *header{reinterpret_cast<MIDIHDR *>(param1)};
    if (message == MIM_LONGDATA)
      takeSysex(input, reinterpret_cast<const uint8_t *>(header->lpData), header->dwBytesRecorded);
    else
      input.inSysex = false;

    // the buffer goes back for more, unless the port is closing: midiInReset returns
    // every buffer, empty, and one put back then would keep the port from closing
    if (!input.closing && header->dwBytesRecorded > 0)
      ::midiInAddBuffer(input.handle, header, sizeof(MIDIHDR));
  }

  // [callback] a piece of sysex: the message is complete at F7; a realtime byte inside it
  // is a message of its own, and any other status byte ends it unfinished
  static void takeSysex(InputPort &input, const uint8_t *data, DWORD size)
  {
    for (DWORD i = 0; i < size; ++i)
    {
      const auto byte{data[i]};
      if (byte >= 0xf8)
      {
        if (*input.onMessage) (*input.onMessage)(&byte, 1);
        continue;
      }
      if (byte == 0xf0)
      {
        input.sysex.clear();
        input.sysex.push_back(byte);
        input.inSysex = true;
        continue;
      }
      if (!input.inSysex) continue;
      if (byte & 0x80 && byte != 0xf7)
      {
        input.inSysex = false;
        continue;
      }
      if (input.sysex.size() >= maxSysexSize)
      {
        input.inSysex = false;
        continue;
      }
      input.sysex.push_back(byte);
      if (byte == 0xf7)
      {
        input.inSysex = false;
        if (*input.onMessage) (*input.onMessage)(input.sysex.data(), input.sysex.size());
      }
    }
  }

  static bool openInput(InputPort &input, UINT index)
  {
    if (::midiInOpen(&input.handle, index, reinterpret_cast<DWORD_PTR>(&inputCallback),
                     reinterpret_cast<DWORD_PTR>(&input), CALLBACK_FUNCTION) != MMSYSERR_NOERROR)
      return false;

    input.sysex.reserve(sysexBufferSize);
    for (size_t i = 0; i < sysexBuffers; ++i)
    {
      auto &header{input.headers[i]};
      header = {};
      header.lpData = input.buffers[i].data();
      header.dwBufferLength = static_cast<DWORD>(input.buffers[i].size());
      header.dwUser = i;
      if (::midiInPrepareHeader(input.handle, &header, sizeof(header)) != MMSYSERR_NOERROR ||
          ::midiInAddBuffer(input.handle, &header, sizeof(header)) != MMSYSERR_NOERROR)
      {
        closeInput(input);
        return false;
      }
    }

    if (::midiInStart(input.handle) != MMSYSERR_NOERROR)
    {
      closeInput(input);
      return false;
    }
    return true;
  }

  static void closeInput(InputPort &input)
  {
    if (!input.handle) return;
    input.closing = true;
    ::midiInStop(input.handle);
    ::midiInReset(input.handle);
    for (auto &header : input.headers)
    {
      if (header.dwFlags & MHDR_PREPARED) ::midiInUnprepareHeader(input.handle, &header, sizeof(header));
    }
    ::midiInClose(input.handle);
    input.handle = nullptr;
  }

  static void closeOutput(OutputPort &output)
  {
    ::midiOutReset(output.handle);
    ::midiOutClose(output.handle);
    ::CloseHandle(output.done);
  }

  std::vector<std::unique_ptr<InputPort>> inputPorts;

  // send() runs on the sender thread, openOutputs() on the main thread
  std::mutex outputMutex;
  std::vector<OutputPort> outputPorts;
};
}  // namespace

std::unique_ptr<MidiBackend> createWinMmMidiBackend()
{
  return std::make_unique<WinMmMidiBackend>();
}
}  // namespace freeaudio::clap_wrapper::standalone
