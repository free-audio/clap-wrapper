
#include <algorithm>
#include <cassert>
#include "standalone_host.h"
#include <fstream>

#if LIN && CLAP_WRAPPER_STANDALONE_X11
#include "detail/standalone/linux/x11_gui.h"
#endif

#if LIN
#include <pwd.h>
#include <unistd.h>
#include <cstdlib>
#endif

#if WIN
#if CLAP_WRAPPER_HAS_WIN32
#include <Windows.h>
#include <ShlObj.h>
#include <string>
#endif
#endif

namespace freeaudio::clap_wrapper::standalone
{

#if WIN && CLAP_WRAPPER_HAS_WIN32
std::optional<fs::path> getStandaloneSettingsPath()
{
  wchar_t *buffer{nullptr};

  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &buffer)))
  {
    fs::path data{std::wstring(buffer) + fs::path::preferred_separator + L"clap-wrapper-standalone"};
    CoTaskMemFree(buffer);

    if (!fs::exists(data)) fs::create_directory(data);

    return data;
  }

  return std::nullopt;
}
#elif LIN
// the XDG config directory: $XDG_CONFIG_HOME where it is set and absolute (the spec has a
// relative one ignored), else ~/.config, with the home from $HOME or the password database
std::optional<fs::path> getStandaloneSettingsPath()
{
  fs::path config;
  const char *xdg{std::getenv("XDG_CONFIG_HOME")};
  if (xdg && *xdg && fs::path(xdg).is_absolute())
  {
    config = xdg;
  }
  else
  {
    const char *home{std::getenv("HOME")};
    if (!home || !*home)
    {
      const auto *user{::getpwuid(::getuid())};
      home = user ? user->pw_dir : nullptr;
    }
    if (!home || !*home) return std::nullopt;
    config = fs::path(home) / ".config";
  }

  const auto data{config / "clap-wrapper-standalone"};
  std::error_code error;
  fs::create_directories(data, error);
  return data;
}
#elif !MAC
std::optional<fs::path> getStandaloneSettingsPath()
{
  TRACE;
  return std::nullopt;
}
#endif

#if WIN && CLAP_WRAPPER_HAS_WIN32
uint32_t supportedScreenAppearances()
{
  // the settings window's own colours, and the dark title bar windows 10 has from 1809
  return (1u << CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO) |
         (1u << CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT) |
         (1u << CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK);
}
#elif !MAC
uint32_t supportedScreenAppearances()
{
  return 0;
}
#endif

StandaloneHost::~StandaloneHost()
{
  // safety net for shutdown paths which don't run through mainFinish
  deactivatePlugin();

  // a midi output thread still running would end the process from ~thread
  closeMidiOutputPorts();

  // the backends call back into this host from their own threads
  if (audioBackend) audioBackend->stop();
  audioBackend.reset();
  midiBackend.reset();
}

void StandaloneHost::setupAudioBusses(const clap_plugin_t *plugin,
                                      const clap_plugin_audio_ports_t *audioports)
{
  if (!audioports) return;

  inputChannelByBus.clear();
  outputChannelByBus.clear();
  totalInputChannels = 0;
  totalOutputChannels = 0;
  mainInput = 0;
  mainOutput = 0;

  numAudioInputs = audioports->count(plugin, true);
  numAudioOutputs = audioports->count(plugin, false);
  LOGDETAIL("inputs/outputs : {}/{}", numAudioInputs, numAudioOutputs);

  // clapProcess works out of fixed pools: one clap_audio_buffer per bus and
  // one utilityBuffer lane per channel, utilityBufferMaxChannels of each. A
  // release build has no assert to catch a plugin that reports more, so clamp
  // here - surplus busses are dropped and surplus channels truncated to zero -
  // to keep the process loop inside the pools.
  const auto maxChannels = static_cast<uint32_t>(utilityBufferMaxChannels);
  if (numAudioInputs + numAudioOutputs > maxChannels)
  {
    LOGINFO("[ERROR] plugin reports {} audio busses, more than the supported {}; dropping the surplus",
            numAudioInputs + numAudioOutputs, maxChannels);
    numAudioInputs = std::min(numAudioInputs, maxChannels);
    numAudioOutputs = maxChannels - numAudioInputs;
  }

  uint32_t channelBudget{maxChannels};
  clap_audio_port_info_t info;
  for (auto i = 0U; i < numAudioInputs; ++i)
  {
    audioports->get(plugin, i, true, &info);
    auto busChans = std::min(info.channel_count, channelBudget);
    if (busChans != info.channel_count)
    {
      LOGINFO("[ERROR] input bus {} truncated from {} to {} channels; the utility buffer holds {}", i,
              info.channel_count, busChans, maxChannels);
    }
    channelBudget -= busChans;
    inputChannelByBus.push_back(busChans);
    totalInputChannels += busChans;
    if (info.flags & CLAP_AUDIO_PORT_IS_MAIN) mainInput = i;
  }
  for (auto i = 0U; i < numAudioOutputs; ++i)
  {
    audioports->get(plugin, i, false, &info);
    auto busChans = std::min(info.channel_count, channelBudget);
    if (busChans != info.channel_count)
    {
      LOGINFO("[ERROR] output bus {} truncated from {} to {} channels; the utility buffer holds {}", i,
              info.channel_count, busChans, maxChannels);
    }
    channelBudget -= busChans;
    outputChannelByBus.push_back(busChans);
    totalOutputChannels += busChans;
    if (info.flags & CLAP_AUDIO_PORT_IS_MAIN) mainOutput = i;
  }
}

void StandaloneHost::setupMIDIBusses(const clap_plugin_t *plugin,
                                     const clap_plugin_note_ports_t *noteports)
{
  auto numMIDIInPorts = noteports->count(plugin, true);
  if (numMIDIInPorts > 0)
  {
    clap_note_port_info_t info;
    noteports->get(plugin, 0, true, &info);
    if (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
    {
      hasMIDIInput = true;
    }
    if (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP)
    {
      hasClapNoteInput = true;
    }
  }
  // the midi outputs send the plugin's midi events and its note on/off, whichever port
  // they come from
  auto numMIDIOutPorts = noteports->count(plugin, false);
  for (uint32_t i = 0; i < numMIDIOutPorts; ++i)
  {
    clap_note_port_info_t info;
    if (noteports->get(plugin, i, false, &info) &&
        (info.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_CLAP)))
    {
      createsMidiOutput = true;
    }
  }
}

void StandaloneHost::process(const float *const *inputs, uint32_t inputChannels, float *const *outputs,
                             uint32_t outputChannels, uint32_t frameCount)
{
  ClapWrapper::detail::shared::SpinLockGuard processLockGuard(processLock);

  auto silenceOutputs = [&]()
  {
    for (auto ch = 0U; outputs && ch < outputChannels; ++ch)
    {
      memset(outputs[ch], 0, frameCount * sizeof(float));
    }
  };

  if (!running)
  {
    silenceOutputs();
    finishedRunning = true;
    return;
  }

  clap_process process{};
  process.steady_time = steadyTime;
  process.transport = nullptr;  // this is a freefloating host
  process.in_events = &inputEvents;
  process.out_events = &outputEvents;
  process.frames_count = frameCount;
  process.audio_inputs_count = numAudioInputs;
  process.audio_outputs_count = numAudioOutputs;

  assert(frameCount < utilityBufferSize);
  if (frameCount >= utilityBufferSize)
  {
    LOGINFO("frameCount {} is beyond utility buffer size {}", frameCount, utilityBufferSize);
    std::terminate();
  }

  float *bufferChanPtr[utilityBufferMaxChannels]{};
  clap_audio_buffer buffers[utilityBufferMaxChannels]{};  // probably twice as large
  size_t ptrIdx{0};
  size_t bufIdx{0};

  int32_t mainOutIdx{-1}, mainInIdx{-1};
  uint32_t mainInChans{0}, mainOutChans{0};

  process.audio_inputs = &(buffers[0]);
  for (auto inp = 0U; inp < numAudioInputs; ++inp)
  {
    auto busChans = inputChannelByBus[inp];
    buffers[bufIdx].channel_count = busChans;
    buffers[bufIdx].data32 = &(bufferChanPtr[ptrIdx]);

    if (mainInIdx < 0 && inp == mainInput && busChans > 0)
    {
      mainInIdx = (int32_t)ptrIdx;
      mainInChans = busChans;
    }

    for (auto ch = 0U; ch < busChans; ++ch)
    {
      bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
      memset(bufferChanPtr[ptrIdx], 0, frameCount * sizeof(float));
      ptrIdx++;
    }
    bufIdx++;
  }

  process.audio_outputs = &(buffers[bufIdx]);
  for (auto oup = 0U; oup < numAudioOutputs; ++oup)
  {
    auto busChans = outputChannelByBus[oup];
    buffers[bufIdx].channel_count = busChans;
    buffers[bufIdx].data32 = &(bufferChanPtr[ptrIdx]);

    if (mainOutIdx < 0 && oup == mainOutput && busChans > 0)
    {
      mainOutIdx = (int32_t)ptrIdx;
      mainOutChans = busChans;
    }

    for (auto ch = 0U; ch < busChans; ++ch)
    {
      bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
      ptrIdx++;
    }
    bufIdx++;
  }

  // muted leaves the input buffers at the silence written above
  if (mainInIdx >= 0 && inputs && inputChannels > 0 && !inputMuted.load(std::memory_order_relaxed))
  {
    // Route the device channels onto the plugin's main input bus. When the bus is wider
    // than the device the last device channel repeats (a mono device feeds both channels
    // of a stereo bus); when the device is wider the surplus device channels are dropped.
    for (auto ch = 0U; ch < mainInChans; ++ch)
    {
      auto devChan = (ch < inputChannels) ? ch : inputChannels - 1;
      memcpy(utilityBuffer[mainInIdx + ch], inputs[devChan], frameCount * sizeof(float));
    }
  }

  // the midi that came in, in its order, as long as the block has room for events and the
  // arena for sysex: what does not fit waits for the next block, with all that came after
  clearInputEvents();
  for (uint32_t arenaUsed = 0; inputEventSize() < maxEventsPerCycle;)
  {
    const auto size{midiToAudio.nextSize()};
    if (size == 0) break;

    if (midiToAudio.nextStatus() != 0xF0)
    {
      clap_event_midi midi{};
      midi.header.size = sizeof(clap_event_midi);
      midi.header.time = 0;
      midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      midi.header.type = CLAP_EVENT_MIDI;
      midi.header.flags = 0;
      midi.port_index = 0;
      midiToAudio.pop(midi.data);
      pushInputEvent(&(midi.header));
      continue;
    }

    if (arenaUsed + size > sizeof(sysexArena)) break;

    clap_event_midi_sysex sysex{};
    sysex.header.size = sizeof(clap_event_midi_sysex);
    sysex.header.time = 0;
    sysex.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    sysex.header.type = CLAP_EVENT_MIDI_SYSEX;
    sysex.header.flags = 0;
    sysex.port_index = 0;
    sysex.buffer = sysexArena + arenaUsed;
    sysex.size = midiToAudio.pop(sysexArena + arenaUsed);
    arenaUsed += size;
    pushInputEvent(&(sysex.header));
  }

  clapPlugin->_plugin->process(clapPlugin->_plugin, &process);
  steadyTime += frameCount;

  if (mainOutIdx >= 0 && outputs && outputChannels > 0)
  {
    // A mono bus is duplicated onto the first two device channels, anything else
    // maps channel-for-channel. Surplus device channels are silenced rather than
    // fed the last bus channel: devices now open at their full width, so that
    // rule put a stereo plugin's right channel on outputs 3 to 8.
    const bool monoBus{mainOutChans == 1};
    const auto fedChannels{std::min(monoBus ? 2U : mainOutChans, outputChannels)};

    for (auto ch = 0U; ch < fedChannels; ++ch)
    {
      memcpy(outputs[ch], utilityBuffer[mainOutIdx + (monoBus ? 0 : ch)], frameCount * sizeof(float));
    }
    for (auto ch = fedChannels; ch < outputChannels; ++ch)
    {
      memset(outputs[ch], 0, frameCount * sizeof(float));
    }
  }
  else
  {
    silenceOutputs();
  }
}

bool StandaloneHost::gui_can_resize()
{
  if (!clapPlugin) return false;

  auto g = clapPlugin->_ext._gui;
  if (!g) return false;

  auto res = g->can_resize(clapPlugin->_plugin);
  return res;
}

bool StandaloneHost::gui_request_resize(uint32_t width, uint32_t height)
{
  if (onRequestResize)
  {
    return onRequestResize(width, height);
  }
  return false;
}

const char *StandaloneHost::host_get_name()
{
  return "CLAP-Wrapper-As-Standalone";
}

#if LIN

bool StandaloneHost::register_timer(uint32_t period_ms, clap_id *timer_id)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  assert(x11Gui);
  return x11Gui->register_timer(period_ms, timer_id);
#else
  return false;
#endif
}
bool StandaloneHost::unregister_timer(clap_id timer_id)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  assert(x11Gui);
  return x11Gui->unregister_timer(timer_id);
#else
  return false;
#endif
}

bool StandaloneHost::register_fd(int fd, clap_posix_fd_flags_t flags)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  return x11Gui->register_fd(fd, flags);
#else
  return false;
#endif
}
bool StandaloneHost::modify_fd(int fd, clap_posix_fd_flags_t flags)
{
  return true;
}
bool StandaloneHost::unregister_fd(int fd)
{
#if LIN && CLAP_WRAPPER_STANDALONE_X11
  return x11Gui->unregister_fd(fd);
#else
  return false;
#endif
}

#else
bool StandaloneHost::register_timer(uint32_t period_ms, clap_id *timer_id)
{
  return false;
}
bool StandaloneHost::unregister_timer(clap_id timer_id)
{
  return false;
}
#endif

static int64_t clapwrite(const clap_ostream *s, const void *buffer, uint64_t size)
{
  auto ofs = static_cast<std::ofstream *>(s->ctx);
  ofs->write((const char *)buffer, size);
  return size;
}

static int64_t clapread(const struct clap_istream *s, void *buffer, uint64_t size)
{
  auto ifs = static_cast<std::ifstream *>(s->ctx);

  // Oh this API is so terrible. I think this is right?
  ifs->read(static_cast<char *>(buffer), size);
  if (ifs->rdstate() == std::ios::goodbit || ifs->rdstate() == std::ios::eofbit) return ifs->gcount();

  if (ifs->rdstate() & std::ios::eofbit) return ifs->gcount();

  return -1;
}

bool StandaloneHost::saveStandaloneAndPluginSettings(const fs::path &intoDir, const fs::path &withName)
{
  // This should obviously be a more robust file format. What we
  // want is an envelope containing the standalone settings and then
  // the streamed plugin data. What we have here is just the streamed
  // plugin data with no settings space for audio port selection etc...

  std::ofstream ofs(intoDir / withName, std::ios::out | std::ios::binary);
  if (!ofs.is_open())
  {
    LOGINFO("[ERROR] Unable to open for writing '{}'", (intoDir / withName).u8string());
    return false;
  }
  if (!clapPlugin || !clapPlugin->_ext._state)
  {
    return false;
  }
  clap_ostream cos{};
  cos.ctx = &ofs;
  cos.write = clapwrite;
  clapPlugin->_ext._state->save(clapPlugin->_plugin, &cos);
  ofs.close();

  return true;
}

bool StandaloneHost::tryLoadStandaloneAndPluginSettings(const fs::path &fromDir,
                                                        const fs::path &withName)
{
  // see comment above on this file format being not just the
  // raw stream in the future
  auto fsp = fromDir / withName;
  std::ifstream ifs(fsp, std::ios::in | std::ios::binary);
  if (!ifs.is_open())
  {
    return false;
  }
  if (!clapPlugin || !clapPlugin->_ext._state)
  {
    return false;
  }
  clap_istream cis{};
  cis.ctx = &ifs;
  cis.read = clapread;
  clapPlugin->_ext._state->load(clapPlugin->_plugin, &cis);
  ifs.close();
  return true;
}

std::optional<fs::path> StandaloneHost::standaloneSettingsFile()
{
  if (!clapPlugin || !clapPlugin->_plugin || !clapPlugin->_plugin->desc) return std::nullopt;

  auto base = getStandaloneSettingsPath();
  if (!base.has_value()) return std::nullopt;

  return *base / clapPlugin->_plugin->desc->id / "standalone-settings.conf";
}

bool StandaloneHost::loadStandaloneSettings()
{
  auto path = standaloneSettingsFile();
  if (!path.has_value()) return false;

  settingsLoaded = settings.load(*path);

  // a plugin that has already set the appearance, from init() say, keeps it
  if (settingsLoaded) setScreenAppearance(settings.screenAppearance, false);

  return settingsLoaded;
}

bool StandaloneHost::setScreenAppearance(uint32_t appearance, bool fromPlugin)
{
  if (appearance >= 32 || !(supportedScreenAppearances() & (1u << appearance))) return false;
  if (!fromPlugin && screenAppearanceFromPlugin) return false;

  screenAppearanceFromPlugin = screenAppearanceFromPlugin || fromPlugin;
  screenAppearance = appearance;
  if (onScreenAppearanceChanged) onScreenAppearanceChanged();
  return true;
}

bool StandaloneHost::saveStandaloneSettings()
{
  auto path = standaloneSettingsFile();
  if (!path.has_value()) return false;

  // The plugin-id directory is shared with the state blobs and usually exists by
  // now, but a first run which never saved state has yet to create it.
  try
  {
    fs::create_directories(path->parent_path());
  }
  catch (const fs::filesystem_error &e)
  {
    LOGINFO("[ERROR] Unable to create settings directory '{}': '{}'", path->parent_path().u8string(),
            e.what());
    return false;
  }

  return settings.save(*path);
}

void StandaloneHost::captureAudioSettings()
{
  settings.audioApiName = audioApiName;
  settings.bufferSize = currentBufferSize;

  // Device names, used flags and the sample rate are deliberately not captured:
  // they describe what could actually be opened (a fallback device, a side that
  // failed to open, a rate the device does not offer), and saveSettings() runs
  // often enough that persisting them would silently replace the user's choice.
  // The frontend writes them at the point of choice instead.
}

void StandaloneHost::applyAudioSettings()
{
  // The API has to be selected first: the devices below are that api's.
  setAudioApi(settings.audioApiName);
  if (!settings.audioApiName.empty() && settings.audioApiName != audioApiName)
  {
    LOGINFO("[WARNING] Audio API '{}' is not available in this build; using '{}'", settings.audioApiName,
            audioApiName);
  }

  audioOutputDevice = resolveOutputDevice(settings.outputDeviceId, settings.outputDeviceName);
  audioInputDevice = resolveInputDevice(settings.inputDeviceId, settings.inputDeviceName);

  // the channels as saved, where the device has them
  const auto inChoice{channelChoiceFor(audioInputDevice, true, settings.inputFirstChannel)};
  const auto outChoice{channelChoiceFor(audioOutputDevice, false, settings.outputFirstChannel)};
  deviceInputFirstChannel = inChoice.first;
  deviceInputChannels = std::max<uint32_t>(inChoice.count, 1);
  deviceOutputFirstChannel = outChoice.first;
  deviceOutputChannels = std::max<uint32_t>(outChoice.count, 1);

  // "did we get a device back" is not the question - "is it a real device" is.
  audioOutputUsed = settings.audioOutputUsed && isKnownDevice(audioOutputDevice);
  audioInputUsed = settings.audioInputUsed && isKnownDevice(audioInputDevice);

  // an input saved as one that is not here falls back to the system default above, as
  // does an input saved with nothing; neither is the user choosing a microphone
  if (audioInputUsed)
  {
    const auto *device{findDevice(audioInputDevice)};
    const bool chosen{
        device &&
        ((!settings.inputDeviceId.empty() && device->id == settings.inputDeviceId) ||
         (!settings.inputDeviceName.empty() && (device->legacyName == settings.inputDeviceName ||
                                                device->name == settings.inputDeviceName)))};
    if (device && !chosen && device->isMicrophone)
    {
      LOGINFO("Not opening '{}': the default input is a microphone, and not the one chosen",
              device->name);
      audioInputUsed = false;
    }
  }

  currentSampleRate = settings.sampleRate;

  // The .conf is hand-editable, and process() terminates the process if a callback
  // delivers utilityBufferSize frames or more, so clamp to the largest size the
  // settings panel offers. 0 still means "use the device default".
  currentBufferSize = settings.bufferSize;
  const auto offered{getBufferSizes()};
  if (!offered.empty())
  {
    const auto largest{*std::max_element(offered.begin(), offered.end())};
    if (currentBufferSize > largest)
    {
      LOGINFO("[WARNING] Buffer size {} from the settings file is beyond the supported {}; using {}",
              currentBufferSize, largest, largest);
      currentBufferSize = largest;
    }
  }

  // The sample rate needs no clamp: startAudioThreadOn() resolves it against the
  // device it is about to open.
}

void StandaloneHost::setChosenDevices(const std::string &outputId, const std::string &inputId)
{
  // the name an older build finds the device by: rtaudio's, where there is one
  auto savedName = [](const AudioDevice &d) { return d.legacyName.empty() ? d.name : d.legacyName; };

  if (const auto *output{findDevice(outputId)})
  {
    settings.outputDeviceId = output->id;
    settings.outputDeviceName = savedName(*output);
  }
  if (const auto *input{findDevice(inputId)})
  {
    settings.inputDeviceId = input->id;
    settings.inputDeviceName = savedName(*input);
  }
}

namespace
{
// the device a saved id and name refer to, as resolveInputDevice describes; null if none
const AudioDevice *savedDevice(const std::vector<AudioDevice> &devices, const std::string &id,
                               const std::string &savedName, bool input)
{
  const auto *byId{findDevice(devices, id, input)};
  if (byId && (savedName.empty() || byId->legacyName == savedName || byId->name == savedName))
    return byId;

  if (const auto *byName{findDevice(devices, savedName, input)}) return byName;
  return byId;
}
}  // namespace

std::string StandaloneHost::resolveInputDevice(const std::string &id, const std::string &savedName)
{
  guaranteeAudioBackend();
  if (!audioBackend) return {};

  if (!id.empty() || !savedName.empty())
  {
    if (const auto *device{savedDevice(deviceList(), id, savedName, true)}) return device->id;

    LOGINFO("[WARNING] Input device '{}' is not available; using the default",
            savedName.empty() ? id : savedName);
  }

  return audioBackend->defaultInputDevice();
}

std::string StandaloneHost::resolveOutputDevice(const std::string &id, const std::string &savedName)
{
  guaranteeAudioBackend();
  if (!audioBackend) return {};

  if (!id.empty() || !savedName.empty())
  {
    if (const auto *device{savedDevice(deviceList(), id, savedName, false)}) return device->id;

    LOGINFO("[WARNING] Output device '{}' is not available; using the default",
            savedName.empty() ? id : savedName);
  }

  return audioBackend->defaultOutputDevice();
}

bool StandaloneHost::activatePlugin(int32_t sr, int32_t minBlock, int32_t maxBlock)
{
  if (!clapPlugin) return false;

  // no device has run yet, or none is attached
  if (sr <= 0 || maxBlock < minBlock || minBlock < 1)
  {
    LOGINFO("[ERROR] Not activating with sampleRate={} blockBounds={} to {}", sr, minBlock, maxBlock);
    return false;
  }

  deactivatePlugin();

  LOGINFO("Activating plugin : sampleRate={} blockBounds={} to {}", sr, minBlock, maxBlock);
  clapPlugin->setSampleRate(sr);
  clapPlugin->setBlockSizes(minBlock, maxBlock);
  if (!clapPlugin->activate())
  {
    LOGINFO("[ERROR] Plugin activate() failed; plugin remains deactivated");
    return false;
  }
  isActive = true;

  if (!clapPlugin->start_processing())
  {
    // CLAP: a false return means processing never started, so process() must not
    // be called and stop_processing() must not be paired with it. Take the
    // activation we just made back down rather than hand the audio callback a
    // plugin that is not allowed to render - deactivatePlugin() skips the
    // stop_processing() because isProcessing was never set.
    LOGINFO("[ERROR] Plugin start_processing() failed; plugin remains deactivated");
    deactivatePlugin();
    return false;
  }
  isProcessing = true;

  // Only now let the callback back in. Taking the lock keeps a callback which is
  // mid-pass from seeing running flip to true against a half-built state.
  {
    ClapWrapper::detail::shared::SpinLockGuard g(processLock);
    finishedRunning = false;
    running = true;
  }

  return true;
}

void StandaloneHost::deactivatePlugin()
{
  // activate/deactivate has to stay balanced, so only ever deactivate what we
  // know we activated. Without an audio device the plugin never gets activated
  // at all, and the shutdown path would otherwise deactivate it regardless.
  if (!clapPlugin || !isActive) return;

  // Stop the callback and wait for it to say so before touching the plugin.
  // Without this, any caller other than the Windows restart handler - an API
  // switch, a device change, OK in a settings panel - could deactivate() while
  // process() was in flight. Making the handshake part of deactivate is what
  // stops each frontend from having to remember it.
  quiesceProcessing();

  if (isProcessing)
  {
    clapPlugin->stop_processing();
    isProcessing = false;
  }

  LOGINFO("Deactivating plugin");
  clapPlugin->deactivate();
  isActive = false;
}

}  // namespace freeaudio::clap_wrapper::standalone
