

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

//#define MINIAUDIO_IMPLEMENTATION
//#include "miniaudio.h"
#include "RtAudio.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

#include "standalone_host.h"

namespace freeaudio::clap_wrapper::standalone
{
int rtaCallback(void *outputBuffer, void *inputBuffer, unsigned int nBufferFrames,
                double /* streamTime */, RtAudioStreamStatus status, void *data)
{
  if (status)
  {
  }
  auto sh = (StandaloneHost *)data;
  sh->clapProcess(outputBuffer, inputBuffer, nBufferFrames);

  return 0;
}

void rtaErrorCallback(RtAudioErrorType, const std::string &errorText)
{
  LOG << "[ERROR] RtAudio reports '" << errorText << "'" << std::endl;
}

void StandaloneHost::startAudioThread()
{
  rtaDac = std::make_unique<RtAudio>(RtAudio::UNSPECIFIED, &rtaErrorCallback);
  rtaDac->showWarnings(true);
  auto dids = rtaDac->getDeviceIds();
  auto dnms = rtaDac->getDeviceNames();

  RtAudio::StreamParameters oParams;
  oParams.nChannels = 2;
  oParams.firstChannel = 0;
  oParams.deviceId = rtaDac->getDefaultOutputDevice();

  RtAudio::StreamParameters iParams;
  iParams.nChannels = 2;
  iParams.firstChannel = 0;
  iParams.deviceId = rtaDac->getDefaultInputDevice();

  LOG << "RtAudio Attached Devices" << std::endl;
  for (auto i = 0U; i < dids.size(); ++i)
  {
    if (oParams.deviceId == dids[i]) LOG << "  - Output : '" << dnms[i] << "'" << std::endl;
  }
  if (numAudioOutputs > 0)
    for (auto i = 0U; i < dids.size(); ++i)
    {
      if (iParams.deviceId == dids[i]) LOG << "  - Input : '" << dnms[i] << "'" << std::endl;
    }

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_SCHEDULE_REALTIME;

  uint32_t bufferFrames{256};
  if (rtaDac->openStream(&oParams, (numAudioInputs > 0) ? &iParams : nullptr, RTAUDIO_FLOAT32, 48000,
                         &bufferFrames, &rtaCallback, (void *)this, &options))
  {
    LOG << "[ERROR]" << rtaDac->getErrorText() << std::endl;
    return;
  }

  if (!rtaDac->isStreamOpen())
  {
    return;
  }

  if (!rtaDac->startStream())
  {
    return;
  }

  LOG << "RTA Started Stream" << std::endl;
}

void StandaloneHost::stopAudioThread()
{
  LOG << "Shutting down audio" << std::endl;
  running = false;

  // bit of a hack. Wait until we get an ack from audio callback
  for (auto i = 0; i < 10000 && !finishedRunning; ++i)
  {
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(1ms);
    // todo put a sleep here
  }
  LOG << "Audio Thread acknowledges shutdown" << std::endl;

  if (rtaDac && rtaDac->isStreamRunning()) rtaDac->stopStream();
  LOG << "RtAudio stream stopped" << std::endl;
  return;
}
}  // namespace freeaudio::clap_wrapper::standalone