#include "standalone_host.h"
#include "standalone_details.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace freeaudio::clap_wrapper::standalone
{

void StandaloneHost::guaranteeMidiBackend()
{
  if (midiBackend) return;

  midiBackend = createMidiBackend();
  if (!midiBackend)
  {
    LOGINFO("[ERROR] This build has no MIDI api");
    return;
  }

  midiBackend->onMessage = [this](const uint8_t *data, size_t size) { processMIDIMessage(data, size); };
  // the port lists follow on the next poll, with the audio devices'
  midiBackend->onPortsChanged = [this]() { devicesChangedPending = true; };
}

std::vector<MidiPort> StandaloneHost::getMidiInputPorts()
{
  guaranteeMidiBackend();
  return midiBackend ? midiBackend->inputs() : std::vector<MidiPort>{};
}

std::vector<MidiPort> StandaloneHost::getMidiOutputPorts()
{
  guaranteeMidiBackend();
  return midiBackend ? midiBackend->outputs() : std::vector<MidiPort>{};
}

namespace
{
// the saved selection against these ports: ids are taken where their port has a saved
// name, since an older build rewrites the names and keeps the ids it does not know as
// they were; names are taken for what is left. a saved name found nowhere is absent
void resolveSaved(const std::vector<MidiPort> &ports, const std::vector<std::string> &names,
                  const std::vector<std::string> &ids, std::vector<std::string> &presentIds,
                  std::vector<std::string> &absentNames)
{
  presentIds.clear();
  absentNames.clear();

  auto add = [&presentIds](const std::string &id)
  {
    if (std::find(presentIds.begin(), presentIds.end(), id) == presentIds.end())
      presentIds.push_back(id);
  };

  std::vector<std::string> namesLeft;
  for (const auto &name : names)
  {
    bool byId{false};
    for (const auto &id : ids)
    {
      const auto *port{findMidiPort(ports, id)};
      if (port && port->id == id && (port->legacyName == name || port->name == name))
      {
        add(port->id);
        byId = true;
        break;
      }
    }
    if (!byId) namesLeft.push_back(name);
  }

  for (const auto &name : namesLeft)
  {
    if (const auto *port{findMidiPort(ports, name)})
      add(port->id);
    else
      absentNames.push_back(name);
  }
}
}  // namespace

void StandaloneHost::savedMidiSelection(bool input, std::vector<std::string> &presentIds,
                                        std::vector<std::string> &absentNames)
{
  resolveSaved(input ? getMidiInputPorts() : getMidiOutputPorts(),
               input ? settings.midiPortNames : settings.midiOutputPortNames,
               input ? settings.midiPortIds : settings.midiOutputPortIds, presentIds, absentNames);
}

void StandaloneHost::setChosenMidiPorts(bool input, const std::vector<std::string> &references)
{
  const auto ports{input ? getMidiInputPorts() : getMidiOutputPorts()};
  auto &names{input ? settings.midiPortNames : settings.midiOutputPortNames};
  auto &ids{input ? settings.midiPortIds : settings.midiOutputPortIds};

  names.clear();
  ids.clear();
  for (const auto &reference : references)
  {
    if (const auto *port{findMidiPort(ports, reference)})
    {
      // the name an older build finds the port by: rtmidi's, where there is one
      names.push_back(port->legacyName.empty() ? port->name : port->legacyName);
      ids.push_back(port->id);
    }
    else
    {
      // saved before and not plugged in now: kept as it was saved
      names.push_back(reference);
    }
  }
  if (input) settings.midiBindAllPorts = false;
}

void StandaloneHost::openMidiPorts(const std::vector<std::string> &references, bool bindAll)
{
  closeMidiPorts();

  // a plugin with no note input has no port for what these would deliver; the
  // selection stays in the settings, untouched
  if (!(hasMIDIInput || hasClapNoteInput)) return;

  guaranteeMidiBackend();
  if (!midiBackend) return;

  const auto available{midiBackend->inputs()};

  std::vector<std::string> wanted;
  for (const auto &port : available)
  {
    if (bindAll) wanted.push_back(port.id);
  }
  for (const auto &reference : bindAll ? std::vector<std::string>{} : references)
  {
    if (const auto *port{findMidiPort(available, reference)})
      wanted.push_back(port->id);
    else
      LOGINFO("[WARNING] MIDI input '{}' is not available", reference);
  }

  currentMidiInputIds = midiBackend->openInputs(wanted);
  for (const auto &id : wanted)
  {
    if (std::find(currentMidiInputIds.begin(), currentMidiInputIds.end(), id) ==
        currentMidiInputIds.end())
      LOGINFO("[ERROR] Unable to open MIDI input '{}'", id);
  }
}

void StandaloneHost::closeMidiPorts()
{
  if (midiBackend && !currentMidiInputIds.empty()) midiBackend->openInputs({});
  currentMidiInputIds.clear();
}

bool StandaloneHost::supportsVirtualMidiPorts()
{
  // what the midi api can do; every macos version has coremidi's virtual endpoints
  guaranteeMidiBackend();
  return midiBackend && midiBackend->supportsVirtualPorts();
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
  open = open && supportsVirtualMidiPorts() && (hasMIDIInput || hasClapNoteInput);
  if (!midiBackend || (!open && !midiVirtualInputOpen)) return;

  const auto name{virtualMidiPortName(true)};
  midiVirtualInputOpen = midiBackend->setVirtualInput(open ? name : std::string{}) && open;
  if (open && !midiVirtualInputOpen)
    LOGINFO("[ERROR] Unable to create the virtual MIDI input '{}'", name);
}

void StandaloneHost::openMidiOutputPorts(const std::vector<std::string> &references)
{
  stopMidiOutputThread();
  guaranteeMidiBackend();
  if (!midiBackend) return;

  std::vector<std::string> wanted;
  if (createsMidiOutput)
  {
    const auto available{midiBackend->outputs()};
    for (const auto &reference : references)
    {
      if (const auto *port{findMidiPort(available, reference)})
        wanted.push_back(port->id);
      else
        LOGINFO("[WARNING] MIDI output '{}' is not available", reference);
    }
  }

  currentMidiOutputIds = midiBackend->openOutputs(wanted);
  for (const auto &id : wanted)
  {
    if (std::find(currentMidiOutputIds.begin(), currentMidiOutputIds.end(), id) ==
        currentMidiOutputIds.end())
      LOGINFO("[ERROR] Unable to open MIDI output '{}'", id);
  }

  startMidiOutputThreadIfNeeded();
}

void StandaloneHost::setVirtualMidiOutput(bool open)
{
  stopMidiOutputThread();

  open = open && supportsVirtualMidiPorts() && createsMidiOutput;
  if (midiBackend && (open || midiVirtualOutputOpen))
  {
    const auto name{virtualMidiPortName(false)};
    midiVirtualOutputOpen = midiBackend->setVirtualOutput(open ? name : std::string{}) && open;
    if (open && !midiVirtualOutputOpen)
      LOGINFO("[ERROR] Unable to create the virtual MIDI output '{}'", name);
  }

  startMidiOutputThreadIfNeeded();
}

void StandaloneHost::closeMidiOutputPorts()
{
  stopMidiOutputThread();
  if (midiBackend)
  {
    if (!currentMidiOutputIds.empty()) midiBackend->openOutputs({});
    if (midiVirtualOutputOpen) midiBackend->setVirtualOutput({});
  }
  currentMidiOutputIds.clear();
  midiVirtualOutputOpen = false;
}

void StandaloneHost::stopMidiOutputThread()
{
  midiOutputOpen = false;
  if (midiOutputThread.joinable()) midiOutputThread.join();
}

void StandaloneHost::startMidiOutputThreadIfNeeded()
{
  if (midiOutputThread.joinable() || (currentMidiOutputIds.empty() && !midiVirtualOutputOpen)) return;

  // whatever the audio thread queued before the last stop is stale. nothing is queued
  // while midiOutputOpen is clear, and nothing else pops, so this empties the queue
  midiChunk stale;
  while (midiFromAudioQueue.pop(stale))
  {
  }
  sysexFromAudio.clear();

  midiOutputOpen = true;
  midiOutputThread = std::thread([this]() { midiOutputLoop(); });
}

void StandaloneHost::midiOutputLoop()
{
  using namespace std::chrono_literals;

  // a note held when the port closes would otherwise sound on until the device is
  // switched off, so a port that has seen a note on is sent all notes off on the way out
  bool sentNoteOn{false};

  // to every open output and the virtual one
  auto sendToAll = [this](const unsigned char *message, size_t size)
  { midiBackend->send(message, size); };

  // the plugin's sysex, whole; allocated here, off the audio thread
  std::vector<uint8_t> sysex(decltype(sysexFromAudio)::maxMessage);

  midiChunk ck;
  while (midiOutputOpen)
  {
    while (const auto size{sysexFromAudio.nextSize()})
    {
      sysexFromAudio.pop(sysex.data());
      sendToAll(sysex.data(), size);
    }

    while (midiFromAudioQueue.pop(ck))
    {
      const auto *message = reinterpret_cast<const unsigned char *>(ck.dat);
      const auto size = shortMidiMessageSize(message[0]);
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
    case CLAP_EVENT_MIDI_SYSEX:
    {
      const auto *sysex = reinterpret_cast<const clap_event_midi_sysex_t *>(evt);
      return sysexFromAudio.tryPush(sysex->buffer, sysex->size);
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
  std::vector<std::string> present, absent;

  // a selection saved by names alone - by a build before the ids - gains the ids of the
  // ports it names now, for the next save: rtmidi's windows names have the port's index
  // in them, and another device plugged in before it moves that
  auto addIds = [this, &present, &absent](bool input)
  {
    const auto &names{input ? settings.midiPortNames : settings.midiOutputPortNames};
    const auto &ids{input ? settings.midiPortIds : settings.midiOutputPortIds};
    if (names.empty() || !ids.empty() || (input && settings.midiBindAllPorts)) return;
    savedMidiSelection(input, present, absent);
    auto references{present};
    references.insert(references.end(), absent.begin(), absent.end());
    setChosenMidiPorts(input, references);
  };
  addIds(true);
  addIds(false);

  savedMidiSelection(true, present, absent);
  for (const auto &name : absent)
  {
    (void)name;  // with logging compiled out
    LOGINFO("[WARNING] MIDI input '{}' is not available", name);
  }
  openMidiPorts(present, settings.midiBindAllPorts);
  setVirtualMidiInput(settings.midiVirtualInput);

  savedMidiSelection(false, present, absent);
  for (const auto &name : absent)
  {
    (void)name;  // with logging compiled out
    LOGINFO("[WARNING] MIDI output '{}' is not available", name);
  }
  openMidiOutputPorts(present);
  setVirtualMidiOutput(settings.midiVirtualOutput);
}

void StandaloneHost::processMIDIMessage(const uint8_t *data, size_t size)
{
  if (!data || size == 0) return;

  // active sensing keeps a cable's link alive and means nothing to a plugin; rtmidi
  // dropped it, as it is sent three times a second
  if (data[0] == 0xFE) return;

  if (data[0] == 0xF0)
  {
    // sysex goes to a plugin that takes the midi dialect only
    if (!hasMIDIInput) return;

    ClapWrapper::detail::shared::SpinLockGuard g(midiToAudioPushLock);
    if (!sysexToAudio.tryPush(data, static_cast<uint32_t>(size)))
    {
      const auto dropped{++sysexDropped};
      if (dropped == 1 || dropped % 100 == 0)
        LOGINFO("[WARNING] {} sysex messages dropped: too large, or the plugin is not taking them",
                dropped);
    }
    return;
  }

  if (size > sizeof(midiChunk::dat)) return;

  midiChunk ck;
  memcpy(ck.dat, data, size);

  ClapWrapper::detail::shared::SpinLockGuard g(midiToAudioPushLock);
  midiToAudioQueue.push(ck);
}

void StandaloneHost::stopMIDIThread()
{
  closeMidiPorts();
  setVirtualMidiInput(false);
  closeMidiOutputPorts();
}

}  // namespace freeaudio::clap_wrapper::standalone
