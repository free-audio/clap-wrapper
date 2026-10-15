#include <algorithm>
#include <exception>
#include <limits>

#include "standalone_host.h"
#include "entry.h"

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// The requested rate if the device offers it; the rate it runs at otherwise, or the
// highest it offers up to 48k when it cannot tell. A device whose rate is not ours to
// set always runs at its own.
int32_t rateOfferedBy(const AudioDevice &device, int32_t requested)
{
  const auto &rates = device.sampleRates;
  if (!device.sampleRateSettable && device.currentSampleRate > 0)
    return static_cast<int32_t>(device.currentSampleRate);

  if (requested > 0 &&
      std::find(rates.begin(), rates.end(), static_cast<uint32_t>(requested)) != rates.end())
    return requested;

  if (device.currentSampleRate > 0) return static_cast<int32_t>(device.currentSampleRate);

  int32_t best{0};
  for (auto rate : rates)
  {
    if (rate <= 48000) best = static_cast<int32_t>(rate);
  }
  if (best == 0 && !rates.empty()) best = static_cast<int32_t>(rates.front());
  return best;
}
}  // namespace

bool StandaloneHost::hasDriverControlPanel()
{
  return audioBackend && audioBackend->hasControlPanel();
}

bool StandaloneHost::showDriverControlPanel()
{
  return audioBackend && audioBackend->isOpen() && audioBackend->showControlPanel();
}

bool StandaloneHost::takeDriverReset()
{
  if (!driverResetRequested) return false;

  // the backend closes it from its own thread, which may not have got there yet
  if (audioBackend && audioBackend->isOpen()) return false;

  driverResetRequested = false;
  return true;
}

void StandaloneHost::guaranteeAudioBackend()
{
  if (!audioBackend)
  {
    LOGDETAIL("Creating the standalone's audio backend");
    setAudioApi({});
  }
}

void StandaloneHost::setAudioApi(const std::string &apiId)
{
  if (audioBackend && audioApiName == apiId) return;

  // Tear the old backend down completely before the new one is constructed.
  // Constructing one may probe the api's drivers, and some of those (ASIO above all)
  // reach into process-global SDK state; doing that while the outgoing backend's
  // realtime thread is still calling us is the WASAPI->ASIO crash.
  if (audioBackend)
  {
    stopAudioThread();
    deactivatePlugin();
    audioBackend.reset();
  }

  audioBackend = createAudioBackend(apiId);
  if (!audioBackend)
  {
    LOGINFO("[ERROR] This build has no audio api");
    audioApiName.clear();
    audioApiDisplayName.clear();
    return;
  }

  // Record what was actually chosen, not what we asked for: an unknown or empty id
  // takes the default, and persisting the request would land somewhere else next time.
  audioApiName = audioBackend->apiId();
  audioApiDisplayName = audioBackend->apiDisplayName();

  // from the backend's threads; acted on by pollAudioEvents() on the main thread
  audioBackend->onEvent = [this](AudioBackendEvent event, const std::string &message)
  {
    switch (event)
    {
      case AudioBackendEvent::StreamStopped:
      {
        LOGINFO("[ERROR] The audio stream stopped: '{}'", message);
        std::lock_guard<std::mutex> guard(streamStoppedLock);
        streamStoppedMessage = message;
        streamStoppedPending = true;
        break;
      }
      case AudioBackendEvent::ResetRequested:
        // not an error to show: an asio driver asked for a reset, and the stream closed
        // for it is opened again (takeDriverReset). a driver that is really gone fails
        // that open, which is reported then
        LOGINFO("The audio driver asked for a reset");
        driverResetRequested = true;
        break;
      case AudioBackendEvent::DevicesChanged:
        devicesChangedPending = true;
        break;
    }
  };

  running = true;
  finishedRunning = false;
}

std::vector<AudioApi> StandaloneHost::getAudioApis()
{
  return audioApis();
}

void StandaloneHost::pollAudioEvents()
{
  if (streamStoppedPending.exchange(false))
  {
    std::string message;
    {
      std::lock_guard<std::mutex> guard(streamStoppedLock);
      message = streamStoppedMessage;
    }

    // the backend has stopped it; this closes it and takes the plugin down with it
    const bool wasActive{isActive};
    stopAudioThread();
    deactivatePlugin();
    currentInputChannels = 0;
    currentOutputChannels = 0;
    lastAudioError = message;
    reopenPending = wasActive;

    if (displayAudioError && !message.empty()) displayAudioError(message);
  }

  // several in a row are one: the list is made again once, here
  if (devicesChangedPending.exchange(false))
  {
    refreshDeviceList();

    // a stream that stopped on its own opens again once its devices are back
    if (reopenPending && !isAudioRunning() && (!audioOutputUsed || isKnownDevice(audioOutputDevice)) &&
        (!audioInputUsed || isKnownDevice(audioInputDevice)))
    {
      LOGINFO("The audio devices are back; opening the stream again");
      {
        SilenceAudioErrors silence(this);
        startAudioThreadOn(audioInputDevice, deviceInputChannels, audioInputUsed && numAudioInputs > 0,
                           audioOutputDevice, deviceOutputChannels,
                           audioOutputUsed && numAudioOutputs > 0, currentSampleRate);
      }
      reopenPending = !isAudioRunning();
    }

    if (onAudioDevicesChanged) onAudioDevicesChanged();
  }
}

uint32_t StandaloneHost::mainBusChannels(bool input) const
{
  const auto &byBus{input ? inputChannelByBus : outputChannelByBus};
  const auto main{input ? mainInput : mainOutput};
  return main < byBus.size() ? byBus[main] : 0;
}

std::vector<AudioChannelChoice> StandaloneHost::getChannelChoices(const std::string &deviceId,
                                                                  bool input)
{
  const auto *device{findDevice(deviceId)};
  if (!device) return {};
  return channelChoices(*device, input, mainBusChannels(input));
}

AudioChannelChoice StandaloneHost::channelChoiceFor(const std::string &deviceId, bool input,
                                                    uint32_t savedFirst)
{
  const auto choices{getChannelChoices(deviceId, input)};
  if (choices.empty()) return {};

  // one the device no longer has - another device took the name, or the bus got wider
  // - is the default, as a missing device is
  for (const auto &choice : choices)
  {
    if (savedFirst > 0 && choice.first == savedFirst - 1) return choice;
  }
  return choices.front();
}

std::string StandaloneHost::defaultInputToOpen()
{
  guaranteeAudioBackend();
  if (!audioBackend) return {};

  const auto systemDefault{audioBackend->defaultInputDevice()};

  // the system default input is usually a laptop's built-in microphone, which feeds the
  // speakers beside it straight back, while an audio interface plugged in is what the
  // user means. not saved: only a choice the user makes is (standalone_settings.h)
  auto preferred{preferredFirstRunInput(deviceList(), systemDefault)};
  if (!preferred.empty())
  {
    LOGINFO("Opening input '{}' rather than the system default", deviceName(preferred));
    return preferred;
  }

  // nor a microphone, which no default opens: with no interface to prefer, this runs
  // without an input until one is chosen
  const auto *device{findDevice(systemDefault)};
  if (!device || device->inputChannels == 0) return {};
  if (device->isMicrophone)
  {
    LOGINFO("Not opening '{}': the default input is a microphone", device->name);
    return {};
  }
  return systemDefault;
}

std::tuple<std::string, std::string, int32_t> StandaloneHost::getDefaultAudioInOutSampleRate()
{
  guaranteeAudioBackend();
  if (!audioBackend) return {};

  auto in{audioBackend->defaultInputDevice()};
  auto out{audioBackend->defaultOutputDevice()};

  // With no output device attached report a rate of 0, and let the caller decide what to
  // do about having no output.
  const auto *outDevice{findDevice(out)};
  if (!outDevice) return {in, out, 0};

  return {in, out, rateOfferedBy(*outDevice, 0)};
}

void StandaloneHost::startAudioThread()
{
  // the system default devices, for a launch with nothing saved or with a saved
  // configuration that does not open
  auto openSystemDefaults = [this]()
  {
    auto [in, out, sr] = getDefaultAudioInOutSampleRate();
    in = defaultInputToOpen();

    audioInputDevice = in;
    audioOutputDevice = out;
    audioInputUsed = isKnownDevice(in);
    audioOutputUsed = isKnownDevice(out);
    const auto inChoice{channelChoiceFor(in, true, 0)};
    const auto outChoice{channelChoiceFor(out, false, 0)};
    deviceInputFirstChannel = inChoice.first;
    deviceInputChannels = std::max<uint32_t>(inChoice.count, 1);
    deviceOutputFirstChannel = outChoice.first;
    deviceOutputChannels = std::max<uint32_t>(outChoice.count, 1);
    currentSampleRate = sr;

    startAudioThreadOn(audioInputDevice, deviceInputChannels, audioInputUsed && numAudioInputs > 0,
                       audioOutputDevice, deviceOutputChannels, audioOutputUsed && numAudioOutputs > 0,
                       currentSampleRate);
  };

  try
  {
    // Persisted configuration wins when we have some; otherwise take the system
    // defaults. A frontend which drives the settings UI itself (Windows) applies
    // them before it gets here and simply calls startAudioThreadOn directly.
    if (loadStandaloneSettings())
    {
      applyAudioSettings();

      // tried quietly. a saved configuration can stop opening - a device's rate was
      // changed, another device took its name, or it is a pair with no rate in common
      // saved before the settings screen refused those - and a launch should still come
      // up with sound. if it does not open, this run uses the system defaults, and only
      // their failure is shown. the file keeps the user's choice, as it does for a
      // device that is not connected
      {
        SilenceAudioErrors silence(this);
        startAudioThreadOn(audioInputDevice, deviceInputChannels, audioInputUsed && numAudioInputs > 0,
                           audioOutputDevice, deviceOutputChannels,
                           audioOutputUsed && numAudioOutputs > 0, currentSampleRate);
      }
      if (isAudioRunning()) return;

      LOGINFO(
          "[WARNING] The saved audio settings did not open ('{}'); using the system "
          "defaults for this run",
          lastAudioError);
    }

    openSystemDefaults();
  }
  catch (const std::exception &e)
  {
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

const std::vector<AudioDevice> &StandaloneHost::deviceList()
{
  static const std::vector<AudioDevice> none;

  guaranteeAudioBackend();
  if (!audioBackend) return none;
  return audioBackend->devices();
}

void StandaloneHost::refreshDeviceList()
{
  guaranteeAudioBackend();
  if (audioBackend) audioBackend->refreshDevices();
}

const AudioDevice *StandaloneHost::findDevice(const std::string &id)
{
  if (id.empty()) return nullptr;

  for (const auto &device : deviceList())
  {
    if (device.id == id) return &device;
  }
  return nullptr;
}

std::string StandaloneHost::deviceName(const std::string &id)
{
  const auto *device{findDevice(id)};
  return device ? device->name : std::string{};
}

std::vector<AudioDevice> StandaloneHost::getInputAudioDevices()
{
  std::vector<AudioDevice> res;
  for (const auto &device : deviceList())
  {
    if (device.inputChannels > 0) res.push_back(device);
  }
  return res;
}

std::vector<AudioDevice> StandaloneHost::getOutputAudioDevices()
{
  std::vector<AudioDevice> res;
  for (const auto &device : deviceList())
  {
    if (device.outputChannels > 0) res.push_back(device);
  }
  return res;
}

std::vector<int32_t> StandaloneHost::getSampleRates()
{
  // The rates the *stream* could run at: the output device's, narrowed to the
  // input device's when both are going to be opened.
  const auto *output{audioOutputUsed ? findDevice(audioOutputDevice) : nullptr};
  const auto *input{audioInputUsed ? findDevice(audioInputDevice) : nullptr};

  std::vector<int32_t> res;
  for (auto rate : commonSampleRates(output, input)) res.push_back(static_cast<int32_t>(rate));
  return res;
}

std::vector<uint32_t> StandaloneHost::getBufferSizes()
{
  // A menu of plausible sizes, narrowed per device where the backend knows its range
  // (getBufferSizesFor). 16 used to head the list and was taken as the Windows first-run
  // default, which is far too small to run reliably on any backend. Powers of two from 32
  // to 512: what a player chooses between, from as little latency as the machine
  // manages to a safe 512; a saved size beyond the last is clamped to it
  // (applyAudioSettings)
  return {32, 64, 128, 256, 512};
}

std::vector<uint32_t> StandaloneHost::getBufferSizesFor(const std::string &outputId,
                                                        const std::string &inputId)
{
  // a size outside a device's range is clamped by it without a word, so none is offered
  uint32_t lowest{0}, highest{std::numeric_limits<uint32_t>::max()};
  for (const auto &id : {outputId, inputId})
  {
    const auto *device{findDevice(id)};
    if (!device || device->maxBufferFrames == 0) continue;
    lowest = std::max(lowest, device->minBufferFrames);
    highest = std::min(highest, device->maxBufferFrames);
  }

  std::vector<uint32_t> res;
  for (auto size : getBufferSizes())
  {
    if (size >= lowest && size <= highest) res.push_back(size);
  }
  return res;
}

void StandaloneHost::startAudioThreadOn(const std::string &inputDevice, uint32_t inputChannels,
                                        bool useInput, const std::string &outputDevice,
                                        uint32_t outputChannels, bool useOutput, int32_t reqSampleRate)
{
  // Nothing in a backend throws, but a plugin's activate() might. Letting that escape
  // would leave the standalone half-started, so trap it here; the plugin simply stays
  // deactivated and the app runs on without audio.
  lastAudioError.clear();

  try
  {
    startAudioThreadOnImpl(inputDevice, inputChannels, useInput, outputDevice, outputChannels, useOutput,
                           reqSampleRate);
  }
  catch (const std::exception &e)
  {
    LOGINFO("[ERROR] Exception starting audio : '{}'", e.what());
    lastAudioError = e.what();
    stopAudioThread();
    deactivatePlugin();
  }
  catch (...)
  {
    LOGINFO("[ERROR] Unknown exception starting audio");
    lastAudioError = "Unknown error while starting audio";
    stopAudioThread();
    deactivatePlugin();
  }

  if (!lastAudioError.empty() && displayAudioError && !audioErrorsAreSilent())
  {
    displayAudioError(lastAudioError);
  }
}

void StandaloneHost::startAudioThreadOnImpl(const std::string &inputDevice, uint32_t inputChannels,
                                            bool useInput, const std::string &outputDevice,
                                            uint32_t outputChannels, bool useOutput,
                                            int32_t reqSampleRate)
{
  guaranteeAudioBackend();
  if (!audioBackend)
  {
    lastAudioError = "This standalone has no audio api";
    return;
  }

  // activatePlugin below is what lets the callback back in, so there is no need
  // to reset running/finishedRunning here.
  stopAudioThread();
  reopenPending = false;

  // The channel counts granted to the previous stream must not outlive it: process()
  // routes by them. They are set again below once the new stream is open.
  currentInputChannels = 0;
  currentOutputChannels = 0;
  currentStream = {};

  audioInputDevice = inputDevice;
  audioInputUsed = useInput;
  audioOutputDevice = outputDevice;
  audioOutputUsed = useOutput;

  // Drop the side we cannot open rather than failing the whole stream: a box with no
  // capture device at all is an entirely ordinary machine.
  const auto *outInfo{useOutput ? findDevice(outputDevice) : nullptr};
  if (useOutput && !outInfo)
  {
    LOGINFO("[ERROR] Output device '{}' is not present; no audio output", outputDevice);
    useOutput = false;
    audioOutputUsed = false;
  }

  const auto *inInfo{useInput ? findDevice(inputDevice) : nullptr};
  if (useInput && !inInfo)
  {
    LOGINFO("[WARNING] Input device '{}' is not present; continuing without audio input", inputDevice);
    useInput = false;
    audioInputUsed = false;
  }

  if (!useOutput && !useInput)
  {
    LOGINFO("[ERROR] Neither an input nor an output device is available; audio is not starting");
    lastAudioError = "No audio input or output device is available";
    return;
  }

  AudioStreamRequest request;
  int32_t sampleRate{reqSampleRate};

  if (useOutput)
  {
    request.outputDevice = outInfo->id;
    request.outputFirstChannel =
        deviceOutputFirstChannel < outInfo->outputChannels ? deviceOutputFirstChannel : 0;
    request.outputChannels =
        std::min(outputChannels, outInfo->outputChannels - request.outputFirstChannel);
    sampleRate = rateOfferedBy(*outInfo, sampleRate);
  }

  if (useInput)
  {
    request.inputDevice = inInfo->id;
    request.inputFirstChannel =
        deviceInputFirstChannel < inInfo->inputChannels ? deviceInputFirstChannel : 0;
    request.inputChannels = std::min(inputChannels, inInfo->inputChannels - request.inputFirstChannel);

    // With no output side nothing else has vetted the rate, and the .conf is
    // hand-editable.
    if (!useOutput) sampleRate = rateOfferedBy(*inInfo, sampleRate);
  }

  if (sampleRate <= 0)
  {
    LOGINFO("[WARNING] No preferred sample rate detected; using 48k");
    sampleRate = 48000;
  }

  if (currentBufferSize == 0)
  {
    currentBufferSize = StandaloneSettings::defaultBufferSize;
  }

  request.sampleRate = static_cast<uint32_t>(sampleRate);
  request.bufferFrames = currentBufferSize;

  // open() says what the device granted, which may not be what we asked for, so
  // everything downstream - activation bounds, the settings we persist, the value shown
  // in a settings panel - reads it back rather than assume the request was honoured.
  AudioStreamInfo granted;
  std::string error;
  if (!audioBackend->open(request, granted, error))
  {
    LOGINFO("[ERROR] Unable to open the audio stream: '{}'", error);
    lastAudioError = error.empty() ? "Unable to open the audio stream" : error;
    return;
  }

  if (granted.sampleRate > 0) sampleRate = static_cast<int32_t>(granted.sampleRate);
  if (granted.bufferFrames > 0) currentBufferSize = granted.bufferFrames;
  currentSampleRate = sampleRate;

  const auto maxFrames{
      std::max<uint32_t>(granted.maxFramesPerCallback, std::max<uint32_t>(currentBufferSize * 2, 1))};
  LOGDETAIL("Audio on '{}': {} Hz, {} frames (at most {} a call), {} in / {} out", audioApiName,
            currentSampleRate, currentBufferSize, granted.maxFramesPerCallback, granted.inputChannels,
            granted.outputChannels);

  if (!activatePlugin(sampleRate, 1, static_cast<int32_t>(maxFrames)))
  {
    LOGINFO("[ERROR] Plugin activation failed; not starting the audio stream");
    lastAudioError = "The plugin failed to activate";
    audioBackend->stop();
    return;
  }

  // what process() routes, set before the first call
  currentStream = granted;
  currentOutputChannels = useOutput ? granted.outputChannels : 0;
  currentInputChannels = useInput ? granted.inputChannels : 0;

  if (!audioBackend->start(*this, error))
  {
    LOGINFO("[ERROR] Unable to start the audio stream: '{}'", error);
    lastAudioError = error.empty() ? "Unable to start the audio stream" : error;
    currentInputChannels = 0;
    currentOutputChannels = 0;
    currentStream = {};
    audioBackend->stop();
    deactivatePlugin();
    return;
  }
}

void StandaloneHost::stopAudioThread()
{
  if (!audioBackend) return;

  if (audioBackend->isRunning())
  {
    LOGINFO("Shutting down audio");

    if (!quiesceProcessing())
    {
      // Shut the stream down anyway: leaving it running is strictly worse than
      // closing one whose last callback we could not confirm.
      LOGINFO("[ERROR] Audio callback did not acknowledge the stop; closing the stream regardless");
    }
  }

  // also closes a stream that is only open, or that the backend stopped on its own
  audioBackend->stop();
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

  if (!audioBackend || !audioBackend->isRunning())
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
