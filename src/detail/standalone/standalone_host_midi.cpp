#include "standalone_host.h"
#include "standalone_details.h"

#include <algorithm>

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
std::vector<std::string> StandaloneHost::getMidiPortNames()
{
  std::vector<std::string> names;

  try
  {
    auto midiIn = std::make_unique<RtMidiIn>();
    numMidiPorts = midiIn->getPortCount();

    for (unsigned int i = 0; i < numMidiPorts; ++i)
    {
      names.push_back(midiIn->getPortName(i));
    }
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

void StandaloneHost::openMidiPorts(const std::vector<std::string> &names, bool bindAll)
{
  stopMIDIThread();

  auto available = getMidiPortNames();

  for (unsigned int port = 0; port < available.size(); ++port)
  {
    if (!bindAll && std::find(names.begin(), names.end(), available[port]) == names.end())
    {
      continue;
    }

    try
    {
      auto midiIn = std::make_unique<RtMidiIn>();
      LOGDETAIL("MIDI: opening '{}'", available[port]);
      midiIn->openPort(port);
      midiIn->setCallback(midiCallback, this);
      midiIns.push_back(std::move(midiIn));
      currentMidiPorts.push_back(port);
      currentMidiPortNames.push_back(available[port]);
    }
    catch (RtMidiError &error)
    {
      LOGINFO("[ERROR] Unable to open MIDI input '{}': '{}'", available[port], error.getMessage());
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

void StandaloneHost::startMIDIThread()
{
  LOGINFO("Initializing Midi");

  // Honour the persisted selection. This used to bind every port unconditionally
  // while the settings UI showed nothing as selected, and it ended the whole
  // process with exit(EXIT_FAILURE) if constructing the first RtMidiIn threw.
  openMidiPorts(settings.midiPortNames, settings.midiBindAllPorts);
}

void StandaloneHost::processMIDIEvents(double deltatime, std::vector<unsigned char> *message)
{
  auto nBytes = message->size();

  if (nBytes <= 3)
  {
    midiChunk ck;
    memset(ck.dat, 0, sizeof(ck.dat));
    memcpy(ck.dat, message->data(), nBytes);
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
  // Resetting the pointers left the vector full of empty slots, so a second call
  // walked a list of nothing and a reopen appended to the stale entries.
  midiIns.clear();
  currentMidiPorts.clear();
  currentMidiPortNames.clear();
}

}  // namespace freeaudio::clap_wrapper::standalone
