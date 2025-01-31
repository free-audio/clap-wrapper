#include "standalone_host.h"
#include "standalone_details.h"

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
void StandaloneHost::startMIDIThread()
{
  try
  {
    LOGINFO("Initializing Midi");
    auto midiIn = std::make_unique<RtMidiIn>();
    numMidiPorts = midiIn->getPortCount();
  }
  catch (RtMidiError &error)
  {
    error.printMessage();
    exit(EXIT_FAILURE);
  }

  LOGDETAIL("MIDI: There are {} MIDI input sources available. Binding all.", numMidiPorts);
  for (unsigned int i = 0; i < numMidiPorts; i++)
  {
    try
    {
      auto midiIn = std::make_unique<RtMidiIn>();
      LOGDETAIL("  - '{}'", midiIn->getPortName(i));
      midiIn->openPort(i);
      midiIn->setCallback(midiCallback, this);
      midiIns.push_back(std::move(midiIn));
    }
    catch (RtMidiError &error)
    {
      error.printMessage();
    }
  }
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
  for (auto &m : midiIns)
  {
    m.reset();
  }
}

}  // namespace freeaudio::clap_wrapper::standalone
