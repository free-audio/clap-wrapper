

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
#include "entry.h"

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

void rtaErrorCallback(RtAudioErrorType errorType, const std::string &errorText)
{
  if (errorType != RTAUDIO_OUTPUT_UNDERFLOW && errorType != RTAUDIO_INPUT_OVERFLOW)
  {
    LOG << "[ERROR] RtAudio reports '" << errorText << "'"
        << " " << errorType << std::endl;
    auto ae = getStandaloneHost()->displayAudioError;
    if (ae)
    {
      ae(errorText);
    }
  }
  else
  {
    static bool reported = false;
    if (!reported)
    {
      LOG << "[ERROR] RtAudio reports '" << errorText << "'" << std::endl;
      LOG << "[ERROR] Supressing future underflow reports" << std::endl;
      reported = true;
    }
  }
}

void StandaloneHost::guaranteeRtAudioDAC()
{
  if (!rtaDac)
  {
    LOG << "Creating RtAudio DAC" << std::endl;
    rtaDac = std::make_unique<RtAudio>(RtAudio::UNSPECIFIED, &rtaErrorCallback);
    rtaDac->showWarnings(true);
  }
}

std::tuple<unsigned int, unsigned int, int32_t> StandaloneHost::getDefaultAudioInOutSampleRate()
{
  guaranteeRtAudioDAC();
  auto iid = rtaDac->getDefaultInputDevice();
  auto oid = rtaDac->getDefaultOutputDevice();
  auto outInfo = rtaDac->getDeviceInfo(oid);
  auto sr = outInfo.preferredSampleRate;

  return {iid, oid, (int32_t)sr};
}
void StandaloneHost::startAudioThread()
{
  guaranteeRtAudioDAC();

  if (startupAudioSet)
  {
    auto in = startAudioIn;
    auto out = startAudioOut;
    auto sr = startSampleRate;
    startAudioThreadOn(in, 2, in > 0 && numAudioInputs > 0, out, 2, out > 0 && numAudioOutputs > 0, sr);
  }
  else
  {
    auto [in, out, sr] = getDefaultAudioInOutSampleRate();
    startAudioThreadOn(in, 2, numAudioInputs > 0, out, 2, numAudioOutputs > 0, sr);
  }
}

std::vector<RtAudio::DeviceInfo> filterDevicesBy(const std::unique_ptr<RtAudio> &rtaDac,
                                                 std::function<bool(const RtAudio::DeviceInfo &)> f)
{
  std::vector<RtAudio::DeviceInfo> res;
  auto dids = rtaDac->getDeviceIds();
  auto dnms = rtaDac->getDeviceNames();
  for (auto d : dids)
  {
    auto inf = rtaDac->getDeviceInfo(d);
    if (f(inf))
    {
      res.push_back(inf);
    }
  }
  return res;
}

std::vector<RtAudio::DeviceInfo> StandaloneHost::getInputAudioDevices()
{
  guaranteeRtAudioDAC();
  return filterDevicesBy(rtaDac, [](auto &a) { return a.inputChannels > 0; });
}

std::vector<RtAudio::DeviceInfo> StandaloneHost::getOutputAudioDevices()
{
  guaranteeRtAudioDAC();
  return filterDevicesBy(rtaDac, [](auto &a) { return a.outputChannels > 0; });
}

void StandaloneHost::startAudioThreadOn(unsigned int inputDeviceID, uint32_t inputChannels,
                                        bool useInput, unsigned int outputDeviceID,
                                        uint32_t outputChannels, bool useOutput, int32_t reqSampleRate)
{
  guaranteeRtAudioDAC();

  if (rtaDac->isStreamRunning())
  {
    stopAudioThread();
    running = true;
    finishedRunning = false;
  }

  audioInputDeviceID = inputDeviceID;
  audioInputUsed = useInput;
  audioOutputDeviceID = outputDeviceID;
  audioOutputUsed = useOutput;

  auto dids = rtaDac->getDeviceIds();
  auto dnms = rtaDac->getDeviceNames();

  RtAudio::StreamParameters oParams;
  int32_t sampleRate{reqSampleRate};

  if (useOutput)
  {
    oParams.deviceId = outputDeviceID;
    auto outInfo = rtaDac->getDeviceInfo(oParams.deviceId);
    oParams.nChannels = std::min(outputChannels, outInfo.outputChannels);
    oParams.firstChannel = 0;
    if (sampleRate < 0)
    {
      sampleRate = outInfo.preferredSampleRate;
    }
    else
    {
      // Mkae sure this sample rate is available
      bool isPossible{false};
      for (auto sr : outInfo.sampleRates)
      {
        isPossible = isPossible || ((int)sr == (int)sampleRate);
      }
      if (!isPossible)
      {
        sampleRate = outInfo.preferredSampleRate;
      }
    }
  }

  RtAudio::StreamParameters iParams;
  if (useInput)
  {
    iParams.deviceId = inputDeviceID;
    auto inInfo = rtaDac->getDeviceInfo(iParams.deviceId);
    iParams.nChannels = std::min(inputChannels, inInfo.inputChannels);
    iParams.firstChannel = 0;
    if (sampleRate < 0) sampleRate = inInfo.preferredSampleRate;
  }

  if (sampleRate < 0)
  {
    LOG << "No preferred sample rate detected; using 48k" << std::endl;
    sampleRate = 48000;
  }

  currentSampleRate = sampleRate;

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_SCHEDULE_REALTIME;

  /*
   * RTAudio doesn't tell you what the possible frame sizes are but instead
   * just tells you to try open stream with power of twos you want. So leave
   * this for now at 256 and return to it shortly.
   */
  LOG << "[WARNING] Hardcoding frame size to 256 samples for now" << std::endl;
  uint32_t bufferFrames{256};
  if (rtaDac->openStream((useOutput) ? &oParams : nullptr, (useInput) ? &iParams : nullptr,
                         RTAUDIO_FLOAT32, sampleRate, &bufferFrames, &rtaCallback, (void *)this,
                         &options))
  {
    LOG << "[ERROR]" << rtaDac->getErrorText() << std::endl;
    rtaDac->closeStream();
    return;
  }

  activatePlugin(sampleRate, 1, bufferFrames * 2);

  LOG << "RtAudio Attached Devices" << std::endl;
  if (useOutput)
  {
    for (auto i = 0U; i < dids.size(); ++i)
    {
      if (oParams.deviceId == dids[i]) LOG << "  - Output : '" << dnms[i] << "'" << std::endl;
    }
  }
  if (useInput)
  {
    for (auto i = 0U; i < dids.size(); ++i)
    {
      if (iParams.deviceId == dids[i]) LOG << "  - Input : '" << dnms[i] << "'" << std::endl;
    }
  }

  if (!rtaDac->isStreamOpen())
  {
    LOG << "[ERROR] Stream failed to open : " << rtaDac->getErrorText() << std::endl;
    return;
  }

  if (rtaDac->startStream())
  {
    LOG << "[ERROR] startStream failed : " << rtaDac->getErrorText() << std::endl;
    return;
  }

  LOG << "RtAudio: Started Stream" << std::endl;
}

void StandaloneHost::stopAudioThread()
{
  LOG << "Shutting down audio" << std::endl;
  if (!rtaDac->isStreamRunning())
  {
    LOG << "Stream not running" << std::endl;
  }
  else
  {
    running = false;

    // bit of a hack. Wait until we get an ack from audio callback
    for (auto i = 0; i < 10000 && !finishedRunning; ++i)
    {
      using namespace std::chrono_literals;
      std::this_thread::sleep_for(1ms);
    }
    LOG << "Audio Thread acknowledges shutdown" << std::endl;

    if (rtaDac && rtaDac->isStreamRunning())
    {
      rtaDac->stopStream();
      rtaDac->closeStream();
    }
    LOG << "RtAudio stream stopped" << std::endl;
  }
  return;
}
}  // namespace freeaudio::clap_wrapper::standalone