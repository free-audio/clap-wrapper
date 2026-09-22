

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

#include <algorithm>
#include <exception>

#include "standalone_host.h"
#include "entry.h"

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// The requested rate if the device offers it, its preferred rate otherwise.
int32_t rateOfferedBy(const RtAudio::DeviceInfo &info, int32_t requested)
{
  if (requested > 0 && std::find(info.sampleRates.begin(), info.sampleRates.end(),
                                 static_cast<unsigned int>(requested)) != info.sampleRates.end())
  {
    return requested;
  }

  return static_cast<int32_t>(info.preferredSampleRate);
}
}  // namespace

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
  if (errorType == RTAUDIO_OUTPUT_UNDERFLOW || errorType == RTAUDIO_INPUT_OVERFLOW)
  {
    static bool reported = false;
    if (!reported)
    {
      LOGINFO("[ERROR] RtAudio reports under/overflow '{}' [{}]", errorText, (int)errorType);
      reported = true;
    }
    return;
  }

  LOGINFO("[ERROR] RtAudio reports '{}' [{}]", errorText, (int)errorType);

  auto sh = getStandaloneHost();

  // Everything is logged; deciding what to *show* is a separate question. RtAudio
  // raises errors from device enumeration, not just from opening a stream, and we
  // enumerate constantly while populating a settings panel. On a machine with no
  // audio devices at all that meant one modal dialog per probe, stacked up on the
  // message loop - which reads to the user as a hang, not as an error.
  if (sh->audioErrorsAreSilent())
  {
    return;
  }

  auto ae = sh->displayAudioError;
  if (ae)
  {
    LOGINFO("[ERROR] Surfacing to the user: '{}'", errorText);
    ae(errorText);
  }
}

void StandaloneHost::guaranteeRtAudioDAC()
{
  if (!rtaDac)
  {
    LOGDETAIL("Creating Standalone RtAudioDAC");
    setAudioApi(RtAudio::Api::UNSPECIFIED);
  }
}

void StandaloneHost::setAudioApi(RtAudio::Api api)
{
  if (rtaDac && audioApi == api) return;

  // Tear the old backend down completely before the new one is constructed.
  // Constructing an RtAudio probes the backend's drivers, and some of those
  // (ASIO above all) reach into process-global SDK state; doing that while the
  // outgoing backend's realtime thread is still calling us is the WASAPI->ASIO
  // crash. Destroying the old instance by assigning over it was worse still:
  // no stop, no running=false handshake, and the isStreamRunning() guard in
  // startAudioThreadOnImpl could not help because by then rtaDac already
  // pointed at the new instance.
  if (rtaDac)
  {
    stopAudioThread();
    deactivatePlugin();
    rtaDac.reset();
  }

  // Device ids belong to the instance that enumerated them, so the cached list
  // means nothing once the backend changes.
  invalidateDeviceList();

  rtaDac = std::make_unique<RtAudio>(api, &rtaErrorCallback);

  // Record what RtAudio actually chose, not what we asked for. Asking for
  // UNSPECIFIED means "you pick", and persisting that would re-run the probe
  // (and possibly land somewhere else) on the next launch.
  audioApi = rtaDac->getCurrentApi();
  audioApiName = RtAudio::getApiName(audioApi);
  audioApiDisplayName = RtAudio::getApiDisplayName(audioApi);
  rtaDac->showWarnings(true);

  running = true;
  finishedRunning = false;
}

std::tuple<unsigned int, unsigned int, int32_t> StandaloneHost::getDefaultAudioInOutSampleRate()
{
  guaranteeRtAudioDAC();
  SilenceAudioErrors silence(this);

  auto iid = rtaDac->getDefaultInputDevice();
  auto oid = rtaDac->getDefaultOutputDevice();

  // With no output device attached RtAudio still returns a default id, and asking
  // it for info raises an error the frontend may show to the user. Report a rate
  // of 0 instead and let the caller decide what to do about having no output.
  if (!isKnownDevice(oid)) return {iid, oid, 0};

  auto outInfo = rtaDac->getDeviceInfo(oid);
  auto sr = outInfo.currentSampleRate;
  if (sr < 1)
  {
    sr = outInfo.preferredSampleRate;
  }

  return {iid, oid, (int32_t)sr};
}
void StandaloneHost::startAudioThread()
{
  try
  {
    // Persisted configuration wins when we have some; otherwise take the system
    // defaults. A frontend which drives the settings UI itself (Windows) applies
    // them before it gets here and simply calls startAudioThreadOn directly.
    if (loadStandaloneSettings())
    {
      applyAudioSettings();
    }
    else
    {
      guaranteeRtAudioDAC();

      auto [in, out, sr] = getDefaultAudioInOutSampleRate();
      audioInputDeviceID = in;
      audioOutputDeviceID = out;
      audioInputUsed = isKnownDevice(in);
      audioOutputUsed = isKnownDevice(out);
      currentSampleRate = sr;
    }

    startAudioThreadOn(audioInputDeviceID, deviceInputChannels, audioInputUsed && numAudioInputs > 0,
                       audioOutputDeviceID, deviceOutputChannels, audioOutputUsed && numAudioOutputs > 0,
                       currentSampleRate);
  }
  catch (const std::exception &e)
  {
    // Device enumeration itself can throw when nothing usable is attached
    LOGINFO("[ERROR] Exception while setting up audio : '{}'", e.what());
    if (displayAudioError) displayAudioError(e.what());
  }
  catch (...)
  {
    // COM errors don't derive from std::exception
    LOGINFO("[ERROR] Unknown exception while setting up audio");
    if (displayAudioError) displayAudioError("Unknown error while setting up audio");
  }
}

std::vector<RtAudio::DeviceInfo> filterDevicesBy(const std::vector<RtAudio::DeviceInfo> &devices,
                                                 std::function<bool(const RtAudio::DeviceInfo &)> f)
{
  std::vector<RtAudio::DeviceInfo> res;
  for (const auto &inf : devices)
  {
    if (f(inf))
    {
      res.push_back(inf);
    }
  }
  return res;
}

std::vector<RtAudio::Api> StandaloneHost::getCompiledApi()
{
  guaranteeRtAudioDAC();

  std::vector<RtAudio::Api> compiledApi;
  rtaDac->getCompiledApi(compiledApi);

  return compiledApi;
}

const std::vector<RtAudio::DeviceInfo> &StandaloneHost::deviceList()
{
  guaranteeRtAudioDAC();

  if (!deviceListValid)
  {
    SilenceAudioErrors silence(this);

    deviceListCache.clear();
    for (auto id : rtaDac->getDeviceIds())
    {
      deviceListCache.push_back(rtaDac->getDeviceInfo(id));
    }

    deviceListValid = true;
  }

  return deviceListCache;
}

std::vector<RtAudio::DeviceInfo> StandaloneHost::getInputAudioDevices()
{
  return filterDevicesBy(deviceList(), [](auto &a) { return a.inputChannels > 0; });
}

std::vector<RtAudio::DeviceInfo> StandaloneHost::getOutputAudioDevices()
{
  return filterDevicesBy(deviceList(), [](auto &a) { return a.outputChannels > 0; });
}

std::vector<int32_t> StandaloneHost::getSampleRates()
{
  guaranteeRtAudioDAC();
  SilenceAudioErrors silence(this);

  std::vector<int32_t> res;

  // The rates the *stream* could run at: the output device's, narrowed to the
  // input device's when both are going to be opened. This used to read the input
  // device's rates only - even when input was unused, and even when the id named
  // no device at all, which raised an RtAudio error the frontend then showed as
  // a modal dialog.
  auto ratesOf = [this](unsigned int deviceID)
  {
    for (const auto &device : deviceList())
    {
      if (device.ID == deviceID) return device.sampleRates;
    }
    return std::vector<unsigned int>{};
  };

  const bool useOutput{audioOutputUsed && isKnownDevice(audioOutputDeviceID)};
  const bool useInput{audioInputUsed && isKnownDevice(audioInputDeviceID)};

  if (!useOutput && !useInput) return res;

  const auto primary{useOutput ? audioOutputDeviceID : audioInputDeviceID};
  for (auto sampleRate : ratesOf(primary))
  {
    res.push_back(static_cast<int32_t>(sampleRate));
  }

  if (useOutput && useInput)
  {
    auto inputRates{ratesOf(audioInputDeviceID)};

    res.erase(std::remove_if(res.begin(), res.end(),
                             [&inputRates](int32_t sampleRate)
                             {
                               return std::find(inputRates.begin(), inputRates.end(),
                                                static_cast<unsigned int>(sampleRate)) ==
                                      inputRates.end();
                             }),
              res.end());
  }

  return res;
}

std::vector<uint32_t> StandaloneHost::getBufferSizes()
{
  guaranteeRtAudioDAC();

  // RtAudio has no way to ask a device what it supports; you find out by trying
  // to open it, and openStream reports back what it actually granted. So this is
  // a menu of plausible sizes, not a claim about the device. 16 used to head the
  // list and was taken as the Windows first-run default, which is far too small
  // to run reliably on any backend.
  std::vector<uint32_t> res{32, 48, 64, 96, 128, 144, 160, 192, 224, 256, 480, 512, 1024, 2048, 4096};
  return res;
}

void StandaloneHost::startAudioThreadOn(unsigned int inputDeviceID, uint32_t inputChannels,
                                        bool useInput, unsigned int outputDeviceID,
                                        uint32_t outputChannels, bool useOutput, int32_t reqSampleRate)
{
  // Backends can throw when no usable device is present. Letting that escape
  // would leave the standalone half-started, so trap it here; the plugin simply
  // stays deactivated and the app runs on without audio.
  try
  {
    startAudioThreadOnImpl(inputDeviceID, inputChannels, useInput, outputDeviceID, outputChannels,
                           useOutput, reqSampleRate);
  }
  catch (const std::exception &e)
  {
    LOGINFO("[ERROR] Exception starting audio : '{}'", e.what());
    deactivatePlugin();
    if (displayAudioError) displayAudioError(e.what());
  }
  catch (...)
  {
    LOGINFO("[ERROR] Unknown exception starting audio");
    deactivatePlugin();
    if (displayAudioError) displayAudioError("Unknown error while starting audio");
  }
}

void StandaloneHost::startAudioThreadOnImpl(unsigned int inputDeviceID, uint32_t inputChannels,
                                            bool useInput, unsigned int outputDeviceID,
                                            uint32_t outputChannels, bool useOutput,
                                            int32_t reqSampleRate)
{
  guaranteeRtAudioDAC();

  // activatePlugin below is what lets the callback back in, so there is no need
  // to reset running/finishedRunning here.
  stopAudioThread();

  // The channel counts granted to the previous stream must not outlive it:
  // rtaCallback sizes its interleaving from them, and a stale non-zero count
  // paired with the null device buffer of a stream that dropped that side
  // would crash the audio thread. They are set again below once the new
  // stream is open.
  currentInputChannels = 0;
  currentOutputChannels = 0;

  audioInputDeviceID = inputDeviceID;
  audioInputUsed = useInput;
  audioOutputDeviceID = outputDeviceID;
  audioOutputUsed = useOutput;

  // getDeviceInfo() on an id RtAudio doesn't know raises an error through the
  // error callback, which frontends put in front of the user as a modal dialog.
  // That happens on entirely ordinary machines: a box with no capture device at
  // all still reports a default input device id of 0, which is not a real device.
  // So check before asking, and drop the side we cannot open rather than failing
  // the whole stream.
  if (useOutput && !isKnownDevice(outputDeviceID))
  {
    LOGINFO("[ERROR] Output device id {} is not present; no audio output", outputDeviceID);
    useOutput = false;
    audioOutputUsed = false;
  }

  if (useInput && !isKnownDevice(inputDeviceID))
  {
    LOGINFO("[WARNING] Input device id {} is not present; continuing without audio input",
            inputDeviceID);
    useInput = false;
    audioInputUsed = false;
  }

  if (!useOutput && !useInput)
  {
    LOGINFO("[ERROR] Neither an input nor an output device is available; audio is not starting");
    return;
  }

  RtAudio::StreamParameters oParams;
  int32_t sampleRate{reqSampleRate};
  RtAudio::DeviceInfo outInfo, inInfo;

  if (useOutput)
  {
    oParams.deviceId = outputDeviceID;
    outInfo = deviceInfoFor(outputDeviceID);
    oParams.nChannels = std::min(outputChannels, outInfo.outputChannels);
    oParams.firstChannel = 0;
    sampleRate = rateOfferedBy(outInfo, sampleRate);
  }

  RtAudio::StreamParameters iParams;
  if (useInput)
  {
    iParams.deviceId = inputDeviceID;
    inInfo = deviceInfoFor(inputDeviceID);
    iParams.nChannels = std::min(inputChannels, inInfo.inputChannels);
    iParams.firstChannel = 0;

    // With no output side nothing else has vetted the rate, and the .conf is
    // hand-editable; openStream() would simply fail and raise a dialog.
    if (!useOutput) sampleRate = rateOfferedBy(inInfo, sampleRate);
  }

  if (sampleRate <= 0)
  {
    LOGINFO("[WARNING] No preferred sample rate detected; using 48k");
    sampleRate = 48000;
  }

  currentSampleRate = sampleRate;

  RtAudio::StreamOptions options;
  options.flags = RTAUDIO_SCHEDULE_REALTIME;

  if (currentBufferSize == 0)
  {
    currentBufferSize = StandaloneSettings::defaultBufferSize;
  }

  // openStream writes the size it actually granted back through this, which may
  // not be what we asked for, so everything downstream - activation bounds, the
  // settings we persist, the value shown in a settings panel - has to read it
  // back rather than assume the request was honoured.
  if (rtaDac->openStream((useOutput) ? &oParams : nullptr, (useInput) ? &iParams : nullptr,
                         RTAUDIO_FLOAT32, sampleRate, &currentBufferSize, &rtaCallback, (void *)this,
                         &options))
  {
    LOGINFO("[ERROR] Error opening rta stream '{}'", rtaDac->getErrorText());
    rtaDac->closeStream();
    return;
  }

  LOGDETAIL("RtAudio granted a buffer size of {} frames", currentBufferSize);

  if (!activatePlugin(sampleRate, 1, currentBufferSize * 2))
  {
    LOGINFO("[ERROR] Plugin activation failed; not starting the audio stream");
    rtaDac->closeStream();
    return;
  }

  if (useOutput)
  {
    currentOutputChannels = oParams.nChannels;
    LOGDETAIL("RtAudio output : '{}', {} channels", outInfo.name, oParams.nChannels);
  }
  if (useInput)
  {
    currentInputChannels = iParams.nChannels;
    LOGDETAIL("RtAudio input : '{}', {} channels", inInfo.name, iParams.nChannels);
  }

  if (!rtaDac->isStreamOpen())
  {
    LOGINFO("[ERROR] Stream failed to open :  {}", rtaDac->getErrorText());
    return;
  }

  if (rtaDac->startStream())
  {
    LOGINFO("[ERROR] startStream failed : {}", rtaDac->getErrorText());
    return;
  }
}

void StandaloneHost::stopAudioThread()
{
  if (!rtaDac) return;

  if (!rtaDac->isStreamRunning())
  {
    // Nothing is running, but the stream may still be open from a failed start.
    if (rtaDac->isStreamOpen()) rtaDac->closeStream();
    return;
  }

  LOGINFO("Shutting down audio");

  if (!quiesceProcessing())
  {
    // Shut the stream down anyway: leaving it running is strictly worse than
    // closing one whose last callback we could not confirm.
    LOGINFO("[ERROR] Audio callback did not acknowledge the stop; closing the stream regardless");
  }

  if (rtaDac->isStreamRunning())
  {
    rtaDac->stopStream();
    rtaDac->closeStream();
  }
}

bool StandaloneHost::quiesceProcessing()
{
  {
    // Setting this under the lock means a callback either observes running==false
    // for its whole pass, or completes the pass it had already begun. It cannot
    // see the flag change mid-process().
    ClapWrapper::detail::shared::SpinLockGuard g(processLock);
    running = false;
  }

  if (!rtaDac || !rtaDac->isStreamRunning())
  {
    // No callback is going to run, so there is no ack coming and none needed.
    return true;
  }

  // Wait for the callback to tell us it has seen the flag. A block can legitimately
  // take a while at large buffer sizes, but two seconds means it is never coming.
  using namespace std::chrono_literals;
  for (auto i = 0; i < 2000 && !finishedRunning; ++i)
  {
    std::this_thread::sleep_for(1ms);
  }

  return finishedRunning;
}
}  // namespace freeaudio::clap_wrapper::standalone
