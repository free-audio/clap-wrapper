#include "standalone_host.h"
#include "entry.h"

#include <algorithm>
#include <cstring>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
constexpr uint32_t numEndpointKinds{4};

void copyString(char *into, size_t size, const std::string &from)
{
  strncpy(into, from.c_str(), size - 1);
  into[size - 1] = 0;
}

clap_wrapper_standalone_endpoint_t audioEndpoint(const RtAudio::DeviceInfo &info, uint32_t channels,
                                                 bool isDefault, bool isBound)
{
  clap_wrapper_standalone_endpoint_t res{};
  copyString(res.id, sizeof(res.id), info.name);
  copyString(res.name, sizeof(res.name), info.name);
  res.channel_count = channels;
  res.flags = (isDefault ? CLAP_WRAPPER_STANDALONE_IS_DEFAULT : 0) |
              (isBound ? CLAP_WRAPPER_STANDALONE_IS_BOUND : 0);
  return res;
}

bool supportsScreen(const clap_host_t *)
{
  return (bool)getStandaloneHost()->onShowSettingsScreen;
}

bool showScreen(const clap_host_t *)
{
  auto *sah = getStandaloneHost();
  if (!sah->onShowSettingsScreen) return false;
  return sah->onShowSettingsScreen();
}

uint32_t countAudioApis(const clap_host_t *)
{
  return (uint32_t)getStandaloneHost()->getCompiledApi().size();
}

bool getAudioApi(const clap_host_t *, uint32_t index, clap_wrapper_standalone_audio_api_t *api)
{
  auto *sah = getStandaloneHost();
  const auto apis = sah->getCompiledApi();
  if (!api || index >= apis.size()) return false;

  const auto a = apis[index];
  *api = {};
  copyString(api->id, sizeof(api->id), RtAudio::getApiName(a));
  copyString(api->name, sizeof(api->name), RtAudio::getApiDisplayName(a));
#if WIN
  // the windows frontend starts a first run on wasapi
  const bool isDefault = a == RtAudio::Api::WINDOWS_WASAPI;
#else
  // rtaudio lists compiled apis in its order of preference
  const bool isDefault = index == 0;
#endif
  api->flags = (isDefault ? CLAP_WRAPPER_STANDALONE_IS_DEFAULT : 0) |
               (a == sah->audioApi ? CLAP_WRAPPER_STANDALONE_IS_BOUND : 0);
  return true;
}

bool supportsListing(const clap_host_t *, uint32_t kind)
{
  // rtmidi output is not wired into the standalone yet
  return kind < CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT;
}

uint32_t countEndpoints(const clap_host_t *, uint32_t kind)
{
  return getStandaloneHost()->snapshotEndpoints(kind);
}

bool getEndpoint(const clap_host_t *, uint32_t kind, uint32_t index,
                 clap_wrapper_standalone_endpoint_t *endpoint)
{
  auto *sah = getStandaloneHost();
  if (kind >= numEndpointKinds || !endpoint) return false;

  const auto &snap = sah->endpointSnapshot[kind];
  if (index >= snap.size()) return false;

  *endpoint = snap[index];
  return true;
}

uint32_t countSampleRates(const clap_host_t *, const char *outputId, const char *inputId)
{
  return getStandaloneHost()->snapshotSampleRates(outputId, inputId);
}

uint32_t getSampleRate(const clap_host_t *, uint32_t index)
{
  const auto &snap = getStandaloneHost()->sampleRateSnapshot;
  return index < snap.size() ? snap[index] : 0;
}

uint32_t countBufferSizes(const clap_host_t *)
{
  return (uint32_t)getStandaloneHost()->getBufferSizes().size();
}

uint32_t getBufferSize(const clap_host_t *, uint32_t index)
{
  const auto sizes = getStandaloneHost()->getBufferSizes();
  return index < sizes.size() ? sizes[index] : 0;
}

bool getAudioStatus(const clap_host_t *, clap_wrapper_standalone_audio_status_t *status)
{
  if (!status) return false;

  auto *sah = getStandaloneHost();
  *status = {};
  status->request_pending = false;
  status->running = sah->isActive && (sah->currentOutputChannels > 0 || sah->currentInputChannels > 0);
  copyString(status->api_id, sizeof(status->api_id), sah->audioApiName);
  if (status->running)
  {
    status->sample_rate = (uint32_t)std::max(sah->currentSampleRate, 0);
    status->buffer_size = sah->currentBufferSize;
    status->input_channels = sah->currentInputChannels;
    status->output_channels = sah->currentOutputChannels;
  }
  copyString(status->last_error, sizeof(status->last_error), sah->lastAudioError);
  return true;
}

bool supportsBinding(const clap_host_t *, uint32_t)
{
  return false;
}

bool requestAudioConfig(const clap_host_t *, const clap_wrapper_standalone_audio_config_t *)
{
  return false;
}

bool requestMidiBindings(const clap_host_t *, uint32_t, const char *const *, uint32_t, bool)
{
  return false;
}

uint32_t supportedResetFlags(const clap_host_t *)
{
  return 0;
}

bool resetStandaloneState(const clap_host_t *, uint32_t)
{
  return false;
}

const clap_wrapper_standalone_features_t features = {
    supportsScreen,      showScreen,          countAudioApis,   getAudioApi,        supportsListing,
    countEndpoints,      getEndpoint,         countSampleRates, getSampleRate,      countBufferSizes,
    getBufferSize,       getAudioStatus,      supportsBinding,  requestAudioConfig, requestMidiBindings,
    supportedResetFlags, resetStandaloneState};
}  // namespace

const clap_wrapper_standalone_features_t *StandaloneHost::standalone_features()
{
  return &features;
}

uint32_t StandaloneHost::snapshotEndpoints(uint32_t kind)
{
  if (kind >= numEndpointKinds) return 0;

  auto &snap = endpointSnapshot[kind];
  snap.clear();

  switch (kind)
  {
    case CLAP_WRAPPER_STANDALONE_AUDIO_INPUT:
      for (const auto &d : getInputAudioDevices())
        snap.push_back(audioEndpoint(d, d.inputChannels, d.isDefaultInput,
                                     d.ID == audioInputDeviceID && currentInputChannels > 0));
      break;
    case CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT:
      for (const auto &d : getOutputAudioDevices())
        snap.push_back(audioEndpoint(d, d.outputChannels, d.isDefaultOutput,
                                     d.ID == audioOutputDeviceID && currentOutputChannels > 0));
      break;
    case CLAP_WRAPPER_STANDALONE_MIDI_INPUT:
      for (const auto &name : getMidiPortNames())
      {
        clap_wrapper_standalone_endpoint_t ep{};
        copyString(ep.id, sizeof(ep.id), name);
        copyString(ep.name, sizeof(ep.name), name);
        if (std::find(currentMidiPortNames.begin(), currentMidiPortNames.end(), name) !=
            currentMidiPortNames.end())
          ep.flags |= CLAP_WRAPPER_STANDALONE_IS_BOUND;
        snap.push_back(ep);
      }
      break;
    default:
      break;
  }

  return (uint32_t)snap.size();
}

uint32_t StandaloneHost::snapshotSampleRates(const char *outputId, const char *inputId)
{
  sampleRateSnapshot.clear();

  // an instrument never opens its input, so the input must not narrow its rates
  const bool useInput = inputId && numAudioInputs > 0;
  if (!outputId && !useInput) return 0;

  std::vector<unsigned int> rates, inputRates;
  if (outputId)
  {
    const auto id = resolveOutputDevice(outputId);
    if (isKnownDevice(id)) rates = deviceInfoFor(id).sampleRates;
  }
  if (useInput)
  {
    const auto id = resolveInputDevice(inputId);
    if (isKnownDevice(id)) inputRates = deviceInfoFor(id).sampleRates;
  }

  if (!outputId)
  {
    rates = inputRates;
  }
  else if (useInput)
  {
    rates.erase(std::remove_if(
                    rates.begin(), rates.end(), [&inputRates](auto r)
                    { return std::find(inputRates.begin(), inputRates.end(), r) == inputRates.end(); }),
                rates.end());
  }

  for (auto r : rates) sampleRateSnapshot.push_back((uint32_t)r);

  return (uint32_t)sampleRateSnapshot.size();
}
}  // namespace freeaudio::clap_wrapper::standalone
