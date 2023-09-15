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

namespace Clap::Standalone
{
void StandaloneHost::startMIDIThread()
{
  unsigned int numPorts{0};
  try
  {
    LOG << "Initializing Midi" << std::endl;
    auto midiIn = std::make_unique<RtMidiIn>();
    numPorts = midiIn->getPortCount();
  }
  catch (RtMidiError &error)
  {
    error.printMessage();
    exit(EXIT_FAILURE);
  }

  LOG << "MIDI: There are " << numPorts << " MIDI input sources available. Binding all.\n";
  for (unsigned int i = 0; i < numPorts; i++)
  {
    try
    {
      auto midiIn = std::make_unique<RtMidiIn>();
      LOG << "  - '" << midiIn->getPortName(i) << "'" << std::endl;
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

  if (nBytes == 3)
  {
    midiChunk ck;
    memcpy(ck.dat, message->data(), 3);
    LOG << "Sending midi to audio thread: dat[0] = " << std::hex << (int)(*message)[0] << std::dec
        << std::endl;
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

}  // namespace Clap::Standalone