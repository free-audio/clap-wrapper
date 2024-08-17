
#include <cassert>
#include "standalone_host.h"
#include <fstream>

#if LIN
#if CLAP_WRAPPER_HAS_GTK3
#include "detail/standalone/linux/gtkutils.h"
#endif
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
#elif !MAC
std::optional<fs::path> getStandaloneSettingsPath()
{
  TRACE;
  return std::nullopt;
}
#endif

StandaloneHost::~StandaloneHost()
{
}

void StandaloneHost::setupAudioBusses(const clap_plugin_t *plugin,
                                      const clap_plugin_audio_ports_t *audioports)
{
  if (!audioports) return;
  numAudioInputs = audioports->count(plugin, true);
  numAudioOutputs = audioports->count(plugin, false);
  LOG << "inputs/outputs : " << numAudioInputs << "/" << numAudioOutputs << std::endl;

  clap_audio_port_info_t info;
  for (auto i = 0U; i < numAudioInputs; ++i)
  {
    audioports->get(plugin, i, true, &info);
    // LOG << "  - input " << i << " " << info.name << std::endl;
    inputChannelByBus.push_back(info.channel_count);
    totalInputChannels += info.channel_count;
    if (info.flags & CLAP_AUDIO_PORT_IS_MAIN) mainInput = i;
  }
  for (auto i = 0U; i < numAudioOutputs; ++i)
  {
    audioports->get(plugin, i, false, &info);
    // LOG << "  - output " << i << " " << info.name << std::endl;
    outputChannelByBus.push_back(info.channel_count);
    totalOutputChannels += info.channel_count;
    if (info.flags & CLAP_AUDIO_PORT_IS_MAIN) mainOutput = i;
  }

  assert(totalOutputChannels + totalInputChannels < utilityBufferMaxChannels);
  if (numAudioInputs > 0) LOG << "main audio input is " << mainInput << std::endl;

  if (numAudioOutputs > 0) LOG << "main audio output is " << mainOutput << std::endl;
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
    LOG << "Set up input: midi=" << hasMIDIInput << " clapNote=" << hasClapNoteInput << std::endl;
  }
  auto numMIDIOutPorts = noteports->count(plugin, false);
  if (numMIDIOutPorts > 0)
  {
    createsMidiOutput = true;
    LOG << "Midi Output not supported yet" << std::endl;
  }
}

void StandaloneHost::clapProcess(void *pOutput, const void *pInput, uint32_t frameCount)
{
  if (!running)
  {
    finishedRunning = true;
    return;
  }

  auto f = (float *)pOutput;
  clap_process process;
  process.transport = nullptr;
  process.in_events = &inputEvents;
  process.out_events = &outputEvents;
  process.frames_count = frameCount;
  process.audio_inputs_count = numAudioInputs;
  process.audio_outputs_count = numAudioOutputs;

  assert(frameCount < utilityBufferSize);
  if (frameCount >= utilityBufferSize)
  {
    LOG << "frameCount " << frameCount << " is beyond utility buffer size " << utilityBufferSize
        << std::endl;
    std::terminate();
  }

  process.audio_outputs_count = 1;

  float *bufferChanPtr[utilityBufferMaxChannels]{};
  clap_audio_buffer buffers[utilityBufferMaxChannels]{};  // probably twice as large
  size_t ptrIdx{0};
  size_t bufIdx{0};

  int32_t mainOutIdx{-1}, mainInIdx{-1};

  process.audio_inputs = &(buffers[0]);
  for (auto inp = 0U; inp < numAudioInputs; ++inp)
  {
    // For now assert sterep
    assert(inputChannelByBus[inp] == 2);
    bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
    memset(bufferChanPtr[ptrIdx], 0, frameCount * sizeof(float));
    ptrIdx++;
    bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
    memset(bufferChanPtr[ptrIdx], 0, frameCount * sizeof(float));
    ptrIdx++;

    buffers[bufIdx].channel_count = 2;
    buffers[bufIdx].data32 = &(bufferChanPtr[ptrIdx - 2]);

    if (mainInIdx < 0)
    {
      // TODO cleaner
      mainInIdx = (int32_t)(ptrIdx - 2);
    }
    bufIdx++;
  }

  process.audio_outputs = &(buffers[bufIdx]);
  for (auto oup = 0U; oup < numAudioOutputs; ++oup)
  {
    // For now assert sterep
    assert(outputChannelByBus[oup] == 2);
    bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
    ptrIdx++;
    bufferChanPtr[ptrIdx] = &(utilityBuffer[ptrIdx][0]);
    ptrIdx++;

    buffers[bufIdx].channel_count = 2;
    buffers[bufIdx].data32 = &(bufferChanPtr[ptrIdx - 2]);

    if (mainOutIdx < 0)
    {
      // TODO cleaner
      mainOutIdx = (int32_t)(ptrIdx - 2);
    }

    bufIdx++;
  }

  if (mainInIdx >= 0 && pInput)
  {
    auto *g = (const float *)pInput;
    for (auto i = 0U; i < frameCount; ++i)
    {
      utilityBuffer[mainInIdx][i] = g[2 * i];
      utilityBuffer[mainInIdx + 1][i] = g[2 * i + 1];
    }
  }

  clearInputEvents();
  clap_event_midi midi;
  midiChunk ck;
  while (midiToAudioQueue.pop(ck))
  {
    midi.port_index = 0;
    midi.header.size = sizeof(clap_event_midi);
    midi.header.time = 0;
    midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    midi.header.type = CLAP_EVENT_MIDI;
    midi.header.flags = 0;
    memcpy(midi.data, ck.dat, sizeof(ck.dat));
    pushInputEvent(&(midi.header));
  }

  clapPlugin->_plugin->process(clapPlugin->_plugin, &process);

  for (auto i = 0U; i < frameCount; ++i)
  {
    f[2 * i] = utilityBuffer[mainOutIdx][i];
    f[2 * i + 1] = utilityBuffer[mainOutIdx + 1][i];
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
#if LIN && CLAP_WRAPPER_HAS_GTK3
  assert(gtkGui);
  return gtkGui->register_timer(period_ms, timer_id);
#else
  return false;
#endif
}
bool StandaloneHost::unregister_timer(clap_id timer_id)
{
#if LIN && CLAP_WRAPPER_HAS_GTK3
  return gtkGui->unregister_timer(timer_id);
#else
  return false;
#endif
}

bool StandaloneHost::register_fd(int fd, clap_posix_fd_flags_t flags)
{
#if LIN && CLAP_WRAPPER_HAS_GTK3
  return gtkGui->register_fd(fd, flags);
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
#if LIN && CLAP_WRAPPER_HAS_GTK3
  return gtkGui->unregister_fd(fd);
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
    LOG << "Unable to open for writing " << (intoDir / withName).u8string() << std::endl;
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
    LOG << "Unable to open for reading " << fsp.u8string() << std::endl;
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

void StandaloneHost::activatePlugin(int32_t sr, int32_t minBlock, int32_t maxBlock)
{
  if (isActive)
  {
    clapPlugin->stop_processing();
    clapPlugin->deactivate();
    isActive = false;
  }

  LOG << "Activating plugin : sampleRate=" << sr << " blockBounds=" << minBlock << " to " << maxBlock
      << std::endl;
  clapPlugin->setSampleRate(sr);
  clapPlugin->setBlockSizes(minBlock, maxBlock);
  clapPlugin->activate();

  clapPlugin->start_processing();

  isActive = true;
}

}  // namespace freeaudio::clap_wrapper::standalone
