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

clap_wrapper_standalone_endpoint_t audioEndpoint(const AudioDevice &device, uint32_t channels,
                                                 bool isDefault, bool isBound)
{
  clap_wrapper_standalone_endpoint_t res{};
  copyString(res.id, sizeof(res.id), device.id);
  copyString(res.name, sizeof(res.name), device.name);
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
  return (uint32_t)getStandaloneHost()->getAudioApis().size();
}

bool getAudioApi(const clap_host_t *, uint32_t index, clap_wrapper_standalone_audio_api_t *api)
{
  auto *sah = getStandaloneHost();
  const auto apis = sah->getAudioApis();
  if (!api || index >= apis.size()) return false;

  const auto &a = apis[index];
  *api = {};
  copyString(api->id, sizeof(api->id), a.id);
  copyString(api->name, sizeof(api->name), a.name);
  // audioApis() lists the default first; the windows frontend starts a first run on it
  const bool isDefault = index == 0;
  api->flags = (isDefault ? CLAP_WRAPPER_STANDALONE_IS_DEFAULT : 0) |
               (a.id == sah->audioApiName ? CLAP_WRAPPER_STANDALONE_IS_BOUND : 0);
  return true;
}

bool supportsListing(const clap_host_t *, uint32_t kind)
{
  return kind < numEndpointKinds;
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

uint32_t countBufferSizes(const clap_host_t *, const char *outputId, const char *inputId)
{
  return getStandaloneHost()->snapshotBufferSizes(outputId, inputId);
}

uint32_t getBufferSize(const clap_host_t *, uint32_t index)
{
  const auto &snap = getStandaloneHost()->bufferSizeSnapshot;
  return index < snap.size() ? snap[index] : 0;
}

bool getAudioStatus(const clap_host_t *, clap_wrapper_standalone_audio_status_t *status)
{
  if (!status) return false;

  auto *sah = getStandaloneHost();
  *status = {};
  status->request_pending = sah->pendingAudioConfig.has_value();
  status->running = sah->isAudioRunning();
  copyString(status->api_id, sizeof(status->api_id), sah->audioApiName);
  if (status->running)
  {
    status->sample_rate = (uint32_t)std::max(sah->currentSampleRate, 0);
    status->buffer_size = sah->currentBufferSize;
    status->input_channels = sah->currentInputChannels;
    status->output_channels = sah->currentOutputChannels;
    status->input_first_channel = sah->currentInputChannels > 0 ? sah->deviceInputFirstChannel : 0;
    status->output_first_channel = sah->currentOutputChannels > 0 ? sah->deviceOutputFirstChannel : 0;
    status->input_latency = sah->currentStream.inputLatency;
    status->output_latency = sah->currentStream.outputLatency;
  }
  copyString(status->last_error, sizeof(status->last_error), sah->lastAudioError);
  status->input_muted = sah->inputMuted.load();
  return true;
}

bool supportsInputMute(const clap_host_t *)
{
  return true;
}

bool setInputMuted(const clap_host_t *, bool muted)
{
  getStandaloneHost()->inputMuted = muted;
  return true;
}

// audio through request_audio_config, midi through request_midi_bindings
bool supportsBinding(const clap_host_t *, uint32_t kind)
{
  return kind < numEndpointKinds;
}

// held for the main loop: StandaloneHost::pollAudioEvents applies it once this call, and
// whatever the plugin was doing when it made it, have returned
bool requestAudioConfig(const clap_host_t *, const clap_wrapper_standalone_audio_config_t *config)
{
  if (!config) return false;

  StandaloneHost::AudioConfigRequest request;
  request.keepApi = config->api_id == nullptr;
  if (config->api_id) request.api = config->api_id;
  request.useInput = config->input_id != nullptr;
  if (config->input_id) request.inputId = config->input_id;
  request.useOutput = config->output_id != nullptr;
  if (config->output_id) request.outputId = config->output_id;
  request.sampleRate = static_cast<int32_t>(config->sample_rate);
  request.bufferSize = config->buffer_size;
  request.inputFirstChannel = config->input_first_channel;
  request.outputFirstChannel = config->output_first_channel;

  getStandaloneHost()->pendingAudioConfig = request;
  return true;
}

// opened at once: unlike an audio change, nothing here stops the stream or touches the
// plugin. kept as the settings screen keeps a choice of ports
bool requestMidiBindings(const clap_host_t *, uint32_t kind, const char *const *ids, uint32_t count,
                         bool bindAll)
{
  if (kind != CLAP_WRAPPER_STANDALONE_MIDI_INPUT && kind != CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT)
    return false;

  auto *sah = getStandaloneHost();
  std::vector<std::string> references;
  if (!bindAll)
  {
    for (uint32_t i = 0; ids && i < count; ++i)
    {
      if (ids[i]) references.emplace_back(ids[i]);
    }
  }

  if (kind == CLAP_WRAPPER_STANDALONE_MIDI_INPUT)
  {
    if (bindAll)
    {
      // a fresh standalone's setting: every port, and those plugged in later
      sah->settings.midiBindAllPorts = true;
      sah->settings.midiPortNames.clear();
      sah->settings.midiPortIds.clear();
    }
    else
    {
      sah->setChosenMidiPorts(true, references);
    }
    sah->openMidiPorts(references, bindAll);
  }
  else
  {
    // outputs keep no "every port" (standalone_settings.h): the ones there now, by id
    if (bindAll)
    {
      for (const auto &port : sah->getMidiOutputPorts()) references.push_back(port.id);
    }
    sah->setChosenMidiPorts(false, references);
    sah->openMidiOutputPorts(references);
  }

  if (!sah->saveStandaloneSettings())
    LOGINFO("[WARNING] Unable to save the MIDI settings; the next launch will not have them");
  return true;
}

uint32_t supportedResetFlags(const clap_host_t *)
{
  return 0;
}

bool resetStandaloneState(const clap_host_t *, uint32_t)
{
  return false;
}

// request_audio_config honours *_first_channel
bool supportsChannelSelection(const clap_host_t *)
{
  return true;
}

bool getEndpointChannelName(const clap_host_t *, uint32_t kind, uint32_t index, uint32_t channel,
                            char *name)
{
  if (!name ||
      (kind != CLAP_WRAPPER_STANDALONE_AUDIO_INPUT && kind != CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT))
    return false;

  auto *sah = getStandaloneHost();
  const auto &snap = sah->endpointSnapshot[kind];
  if (index >= snap.size() || channel >= snap[index].channel_count) return false;

  const auto *device = sah->findDevice(snap[index].id);
  if (!device) return false;

  copyString(name, CLAP_NAME_SIZE,
             channelName(*device, kind == CLAP_WRAPPER_STANDALONE_AUDIO_INPUT, channel));
  return true;
}

uint32_t supportedAppearances(const clap_host_t *)
{
  return supportedScreenAppearances();
}

bool setAppearance(const clap_host_t *, uint32_t appearance)
{
  return getStandaloneHost()->setScreenAppearance(appearance, true);
}

uint32_t getAppearance(const clap_host_t *)
{
  return getStandaloneHost()->screenAppearance;
}

// both, wherever there is a settings folder to keep a session in
uint32_t supportedPersistence(const clap_host_t *)
{
  if (!getStandaloneHost()->settingsFolder.has_value()) return 0;
  return (1u << CLAP_WRAPPER_STANDALONE_STATE_PERSIST_SESSION) |
         (1u << CLAP_WRAPPER_STANDALONE_STATE_PERSIST_NONE);
}

bool setPersistence(const clap_host_t *host, uint32_t persistence)
{
  if (persistence >= 32 || !(supportedPersistence(host) & (1u << persistence))) return false;

  auto *sah = getStandaloneHost();
  sah->pluginStatePersistence = persistence;
  sah->settings.pluginStatePersistence = persistence;
  if (!sah->saveStandaloneSettings())
    LOGINFO("[WARNING] Unable to save the state persistence; the next launch will not have it");
  return true;
}

uint32_t getPersistence(const clap_host_t *)
{
  return getStandaloneHost()->pluginStatePersistence;
}

const clap_wrapper_standalone_features_t features = {
    supportsScreen,         showScreen,           countAudioApis,       getAudioApi,
    supportsListing,        countEndpoints,       getEndpoint,          countSampleRates,
    getSampleRate,          countBufferSizes,     getBufferSize,        getAudioStatus,
    supportsInputMute,      setInputMuted,        supportsBinding,      requestAudioConfig,
    requestMidiBindings,    supportedResetFlags,  resetStandaloneState, supportsChannelSelection,
    getEndpointChannelName, supportedAppearances, setAppearance,        getAppearance,
    supportedPersistence,   setPersistence,       getPersistence};
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
                                     d.id == audioInputDevice && currentInputChannels > 0));
      break;
    case CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT:
      for (const auto &d : getOutputAudioDevices())
        snap.push_back(audioEndpoint(d, d.outputChannels, d.isDefaultOutput,
                                     d.id == audioOutputDevice && currentOutputChannels > 0));
      break;
    case CLAP_WRAPPER_STANDALONE_MIDI_INPUT:
    case CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT:
    {
      const bool input = kind == CLAP_WRAPPER_STANDALONE_MIDI_INPUT;
      const auto &bound = input ? currentMidiInputIds : currentMidiOutputIds;
      for (const auto &port : input ? getMidiInputPorts() : getMidiOutputPorts())
      {
        clap_wrapper_standalone_endpoint_t ep{};
        copyString(ep.id, sizeof(ep.id), port.id);
        copyString(ep.name, sizeof(ep.name), port.name);
        if (std::find(bound.begin(), bound.end(), port.id) != bound.end())
          ep.flags |= CLAP_WRAPPER_STANDALONE_IS_BOUND;
        snap.push_back(ep);
      }
      break;
    }
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

  // ids, or names a plugin stored from before; one not present is the system default's
  const auto *output = outputId ? findDevice(resolveOutputDevice(outputId)) : nullptr;
  const auto *input = useInput ? findDevice(resolveInputDevice(inputId)) : nullptr;

  sampleRateSnapshot = commonSampleRates(output, input);

  return (uint32_t)sampleRateSnapshot.size();
}

uint32_t StandaloneHost::snapshotBufferSizes(const char *outputId, const char *inputId)
{
  // as with the rates, an instrument's input never narrows anything
  const bool useInput = inputId && numAudioInputs > 0;
  const auto output = outputId ? resolveOutputDevice(outputId) : std::string{};
  const auto input = useInput ? resolveInputDevice(inputId) : std::string{};

  bufferSizeSnapshot = getBufferSizesFor(output, input);
  return (uint32_t)bufferSizeSnapshot.size();
}

void StandaloneHost::settingsScreenClosed(uint32_t result)
{
  auto plugin = getMainPlugin();
  if (!plugin || !plugin->_ext._standalone_features) return;

  auto *ext = plugin->_ext._standalone_features;
  if (ext->audio_midi_configuration_screen_closed)
    ext->audio_midi_configuration_screen_closed(plugin->_plugin, result);
}

}  // namespace freeaudio::clap_wrapper::standalone
