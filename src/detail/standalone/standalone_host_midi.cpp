#include "standalone_host.h"
#include "standalone_details.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtMidi.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// the length of a short message from its status byte; 0 for what cannot be sent as one:
// a data byte where the status should be (running status), or the start or end of sysex
size_t shortMessageSize(unsigned char status)
{
  if (status < 0x80) return 0;

  switch (status & 0xF0)
  {
    case 0xC0:
    case 0xD0:
      return 2;
    case 0xF0:
      switch (status)
      {
        case 0xF0:
        case 0xF7:
          return 0;
        case 0xF1:
        case 0xF3:
          return 2;
        case 0xF2:
          return 3;
        default:
          return 1;
      }
    default:
      return 3;
  }
}
// rtmidi's index of the port with this name, -1 if it has none
int portIndex(RtMidi &midi, const std::string &name)
{
  for (unsigned int i = 0; i < midi.getPortCount(); ++i)
    if (midi.getPortName(i) == name) return (int)i;
  return -1;
}
}  // namespace

std::vector<std::string> StandaloneHost::getMidiPortNames()
{
  std::vector<std::string> names;

  try
  {
    auto midiIn = std::make_unique<RtMidiIn>();

    // our own virtual output is a source like any other, and must not be listed as one
    const auto own = midiVirtualOut ? virtualMidiPortName(false) : std::string();
    for (unsigned int i = 0; i < midiIn->getPortCount(); ++i)
    {
      auto name = midiIn->getPortName(i);
      if (name != own) names.push_back(name);
    }
    numMidiPorts = (uint32_t)names.size();
  }
  catch (RtMidiError &error)
  {
    // No MIDI system at all - on Linux this is the routine "ALSA sequencer is not
    // available" case. Report no ports; it is not a reason to end the process.
    LOGINFO("[ERROR] Unable to enumerate MIDI inputs: '{}'", error.getMessage());
    numMidiPorts = 0;
    names.clear();
  }

  return names;
}

std::vector<std::string> StandaloneHost::getMidiOutputPortNames()
{
  std::vector<std::string> names;

  try
  {
    auto midiOut = std::make_unique<RtMidiOut>();

    const auto own = midiVirtualIn ? virtualMidiPortName(true) : std::string();
    for (unsigned int i = 0; i < midiOut->getPortCount(); ++i)
    {
      auto name = midiOut->getPortName(i);
      if (name != own) names.push_back(name);
    }
    numMidiOutputPorts = (uint32_t)names.size();
  }
  catch (RtMidiError &error)
  {
    LOGINFO("[ERROR] Unable to enumerate MIDI outputs: '{}'", error.getMessage());
    numMidiOutputPorts = 0;
    names.clear();
  }

  return names;
}

void StandaloneHost::openMidiPorts(const std::vector<std::string> &names, bool bindAll)
{
  closeMidiPorts();

  // a plugin with no note input has no port for what these would deliver; the
  // selection stays in the settings, untouched
  if (!(hasMIDIInput || hasClapNoteInput)) return;

  auto available = getMidiPortNames();

  for (const auto &name : available)
  {
    if (!bindAll && std::find(names.begin(), names.end(), name) == names.end())
    {
      continue;
    }

    try
    {
      auto midiIn = std::make_unique<RtMidiIn>();
      // by name: the list leaves our own virtual port out, so its positions are not rtmidi's
      const int port = portIndex(*midiIn, name);
      if (port < 0) continue;
      LOGDETAIL("MIDI: opening '{}'", name);
      midiIn->openPort(port);
      midiIn->setCallback(midiCallback, this);
      midiIns.push_back(std::move(midiIn));
      currentMidiPorts.push_back(port);
      currentMidiPortNames.push_back(name);
    }
    catch (RtMidiError &error)
    {
      LOGINFO("[ERROR] Unable to open MIDI input '{}': '{}'", name, error.getMessage());
    }
  }

  // Ports selected in a previous session which are not plugged in now.
  if (!bindAll)
  {
    for (const auto &wanted : names)
    {
      if (std::find(available.begin(), available.end(), wanted) == available.end())
      {
        LOGINFO("[WARNING] MIDI input '{}' is not available", wanted);
      }
    }
  }
}

void StandaloneHost::closeMidiPorts()
{
  // Resetting the pointers left the vector full of empty slots, so a second call
  // walked a list of nothing and a reopen appended to the stale entries.
  midiIns.clear();
  currentMidiPorts.clear();
  currentMidiPortNames.clear();
}

bool StandaloneHost::supportsVirtualMidiPorts()
{
  // what the midi api can do; every macos version has coremidi's virtual endpoints
  std::vector<RtMidi::Api> apis;
  RtMidi::getCompiledApi(apis);
  return std::any_of(
      apis.begin(), apis.end(), [](auto api)
      { return api == RtMidi::MACOSX_CORE || api == RtMidi::LINUX_ALSA || api == RtMidi::UNIX_JACK; });
}

std::string StandaloneHost::virtualMidiPortName(bool input) const
{
  std::string name{"MIDI"};
  if (clapPlugin && clapPlugin->_plugin && clapPlugin->_plugin->desc &&
      clapPlugin->_plugin->desc->name && *clapPlugin->_plugin->desc->name)
    name = clapPlugin->_plugin->desc->name;
  return name + (input ? " Virtual In" : " Virtual Out");
}

void StandaloneHost::setVirtualMidiInput(bool open)
{
  midiVirtualIn.reset();
  if (!open || !supportsVirtualMidiPorts() || !(hasMIDIInput || hasClapNoteInput)) return;

  const auto name = virtualMidiPortName(true);
  try
  {
    auto midiIn = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, name);
    midiIn->openVirtualPort(name);
    midiIn->setCallback(midiCallback, this);
    midiVirtualIn = std::move(midiIn);
  }
  catch (RtMidiError &error)
  {
    LOGINFO("[ERROR] Unable to create the virtual MIDI input '{}': '{}'", name, error.getMessage());
  }
}

void StandaloneHost::openMidiOutputPorts(const std::vector<std::string> &names)
{
  stopMidiOutputThread();
  midiOuts.clear();
  currentMidiOutputPortNames.clear();

  if (createsMidiOutput)
  {
    auto available = getMidiOutputPortNames();

    for (const auto &name : available)
    {
      if (std::find(names.begin(), names.end(), name) == names.end())
      {
        continue;
      }

      try
      {
        auto midiOut = std::make_unique<RtMidiOut>();
        const int port = portIndex(*midiOut, name);
        if (port < 0) continue;
        LOGDETAIL("MIDI: opening output '{}'", name);
        midiOut->openPort(port);
        midiOuts.push_back(std::move(midiOut));
        currentMidiOutputPortNames.push_back(name);
      }
      catch (RtMidiError &error)
      {
        LOGINFO("[ERROR] Unable to open MIDI output '{}': '{}'", name, error.getMessage());
      }
    }

    for (const auto &wanted : names)
    {
      if (std::find(available.begin(), available.end(), wanted) == available.end())
      {
        LOGINFO("[WARNING] MIDI output '{}' is not available", wanted);
      }
    }
  }

  startMidiOutputThreadIfNeeded();
}

void StandaloneHost::setVirtualMidiOutput(bool open)
{
  stopMidiOutputThread();
  midiVirtualOut.reset();

  if (open && supportsVirtualMidiPorts() && createsMidiOutput)
  {
    const auto name = virtualMidiPortName(false);
    try
    {
      auto midiOut = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, name);
      midiOut->openVirtualPort(name);
      midiVirtualOut = std::move(midiOut);
    }
    catch (RtMidiError &error)
    {
      LOGINFO("[ERROR] Unable to create the virtual MIDI output '{}': '{}'", name, error.getMessage());
    }
  }

  startMidiOutputThreadIfNeeded();
}

void StandaloneHost::closeMidiOutputPorts()
{
  stopMidiOutputThread();
  midiOuts.clear();
  currentMidiOutputPortNames.clear();
  midiVirtualOut.reset();
}

void StandaloneHost::stopMidiOutputThread()
{
  midiOutputOpen = false;
  if (midiOutputThread.joinable()) midiOutputThread.join();
}

void StandaloneHost::startMidiOutputThreadIfNeeded()
{
  if (midiOutputThread.joinable() || (midiOuts.empty() && !midiVirtualOut)) return;

  // whatever the audio thread queued before the last stop is stale. nothing is queued
  // while midiOutputOpen is clear, and nothing else pops, so this empties the queue
  midiChunk stale;
  while (midiFromAudioQueue.pop(stale))
  {
  }

  midiOutputOpen = true;
  midiOutputThread = std::thread([this]() { midiOutputLoop(); });
}

void StandaloneHost::midiOutputLoop()
{
  using namespace std::chrono_literals;

  // a note held when the port closes would otherwise sound on until the device is
  // switched off, so a port that has seen a note on is sent all notes off on the way out
  bool sentNoteOn{false};

  auto send = [](RtMidiOut &out, const unsigned char *message, size_t size)
  {
    try
    {
      out.sendMessage(message, size);
    }
    catch (RtMidiError &error)
    {
      LOGINFO("[ERROR] Unable to send MIDI: '{}'", error.getMessage());
    }
  };
  auto sendToAll = [this, &send](const unsigned char *message, size_t size)
  {
    for (auto &out : midiOuts) send(*out, message, size);
    if (midiVirtualOut) send(*midiVirtualOut, message, size);
  };

  midiChunk ck;
  while (midiOutputOpen)
  {
    while (midiFromAudioQueue.pop(ck))
    {
      const auto *message = reinterpret_cast<const unsigned char *>(ck.dat);
      const auto size = shortMessageSize(message[0]);
      if (size == 0) continue;

      if ((message[0] & 0xF0) == 0x90) sentNoteOn = true;
      sendToAll(message, size);
    }

    // the audio thread cannot wake this one without a call that may block, so it polls
    std::this_thread::sleep_for(1ms);
  }

  if (sentNoteOn)
  {
    for (unsigned char channel = 0; channel < 16; ++channel)
    {
      const unsigned char allNotesOff[3]{static_cast<unsigned char>(0xB0 | channel), 123, 0};
      sendToAll(allNotesOff, sizeof(allNotesOff));
    }
  }
}

bool StandaloneHost::outputEventPush(const clap_event_header_t *evt)
{
  // anything else, or nowhere to send it, is taken and dropped, as it always was
  if (!evt || evt->space_id != CLAP_CORE_EVENT_SPACE_ID || !midiOutputOpen) return true;

  midiChunk ck;
  switch (evt->type)
  {
    case CLAP_EVENT_MIDI:
    {
      const auto *midi = reinterpret_cast<const clap_event_midi_t *>(evt);
      memcpy(ck.dat, midi->data, sizeof(ck.dat));
      break;
    }
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    {
      const auto *note = reinterpret_cast<const clap_event_note_t *>(evt);
      // a wildcard channel or key names no single midi note
      if (note->channel < 0 || note->channel > 15 || note->key < 0 || note->key > 127) return true;

      const bool on = evt->type == CLAP_EVENT_NOTE_ON;
      auto velocity = std::clamp((int)std::lround(note->velocity * 127.0), 0, 127);
      // a note on at velocity 0 is a note off in midi
      if (on && velocity == 0) velocity = 1;

      ck.dat[0] = (char)((on ? 0x90 : 0x80) | note->channel);
      ck.dat[1] = (char)note->key;
      ck.dat[2] = (char)velocity;
      break;
    }
    default:
      return true;
  }

  return midiFromAudioQueue.tryPush(ck);
}

void StandaloneHost::startMIDIThread()
{
  LOGINFO("Initializing Midi");

  // Honour the persisted selection. This used to bind every port unconditionally
  // while the settings UI showed nothing as selected, and it ended the whole
  // process with exit(EXIT_FAILURE) if constructing the first RtMidiIn threw.
  openMidiPorts(settings.midiPortNames, settings.midiBindAllPorts);
  setVirtualMidiInput(settings.midiVirtualInput);
  openMidiOutputPorts(settings.midiOutputPortNames);
  setVirtualMidiOutput(settings.midiVirtualOutput);
}

void StandaloneHost::processMIDIEvents(double deltatime, std::vector<unsigned char> *message)
{
  auto nBytes = message->size();

  if (nBytes <= 3)
  {
    midiChunk ck;
    memset(ck.dat, 0, sizeof(ck.dat));
    memcpy(ck.dat, message->data(), nBytes);

    ClapWrapper::detail::shared::SpinLockGuard g(midiToAudioPushLock);
    midiToAudioQueue.push(ck);
  }
}

void StandaloneHost::midiCallback(double deltatime, std::vector<unsigned char> *message, void *userData)
{
  auto sh = (StandaloneHost *)userData;
  sh->processMIDIEvents(deltatime, message);
}

void StandaloneHost::stopMIDIThread()
{
  closeMidiPorts();
  midiVirtualIn.reset();
  closeMidiOutputPorts();
}

}  // namespace freeaudio::clap_wrapper::standalone
