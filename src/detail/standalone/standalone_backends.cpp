#include "standalone_backends.h"
#include "standalone_details.h"

#include <algorithm>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// the native apis, which come first; the first usable one is the default
struct NativeApi
{
  AudioApi api;
  std::unique_ptr<AudioBackend> (*create)();
  // null for an api that is always there; else whether a server answers now
  bool (*usable)();
};

const std::vector<NativeApi> &nativeApis()
{
  static const std::vector<NativeApi> apis{
#if !CLAP_WRAPPER_STANDALONE_RTAUDIO && MAC
      {{"core", "CoreAudio"}, &createCoreAudioBackend, nullptr},
#elif !CLAP_WRAPPER_STANDALONE_RTAUDIO && WIN
      {{"wasapi", "WASAPI"}, &createWasapiBackend, nullptr},
#endif
#if CLAP_WRAPPER_STANDALONE_PIPEWIRE
      {{"pipewire", "PipeWire"}, &createPipeWireBackend, &pipeWireUsable},
#endif
#if CLAP_WRAPPER_STANDALONE_JACK
      {{"jack", "JACK"}, &createJackBackend, &jackUsable},
#endif
  };
  return apis;
}

const NativeApi *nativeApi(const std::string &id)
{
  for (const auto &native : nativeApis())
  {
    if (native.api.id == id) return &native;
  }
  return nullptr;
}

bool usable(const NativeApi &native)
{
  return !native.usable || native.usable();
}
}  // namespace

std::vector<AudioApi> audioApis()
{
  // linux has PipeWire and JACK, either of which may have no server running: the ones
  // that answer come first, in their order, so that a JACK-only machine starts on JACK
  std::vector<AudioApi> res, unanswered;
  for (const auto &native : nativeApis()) (usable(native) ? res : unanswered).push_back(native.api);
  res.insert(res.end(), unanswered.begin(), unanswered.end());
#if CLAP_WRAPPER_STANDALONE_RTAUDIO || CLAP_WRAPPER_STANDALONE_ASIO
  for (const auto &api : rtAudioApis()) res.push_back(api);
#endif
  return res;
}

std::unique_ptr<AudioBackend> createAudioBackend(const std::string &apiId)
{
  const auto apis = audioApis();
  if (apis.empty()) return nullptr;

  auto known = std::find_if(apis.begin(), apis.end(), [&apiId](const auto &a) { return a.id == apiId; });
  if (known == apis.end())
  {
    if (!apiId.empty())
      LOGINFO("[WARNING] Audio API '{}' is not in this build; using the default", apiId);
#if CLAP_WRAPPER_STANDALONE_RTAUDIO
    // rtaudio's own default, which may take the first api that has a device
    return createRtAudioBackend({});
#else
    known = apis.begin();
#endif
  }

  // a saved api without a server, where another has one: the default, which does
  const auto *native{nativeApi(known->id)};
  if (native && known != apis.begin() && native->usable && !native->usable())
  {
    if (const auto *first{nativeApi(apis.front().id)}; first && usable(*first))
    {
      LOGINFO("[WARNING] Audio API '{}' has no server running; using '{}'", known->id, apis.front().id);
      native = first;
    }
  }
  if (native) return native->create();
#if CLAP_WRAPPER_STANDALONE_RTAUDIO || CLAP_WRAPPER_STANDALONE_ASIO
  return createRtAudioBackend(known->id);
#else
  return nullptr;
#endif
}

std::unique_ptr<MidiBackend> createMidiBackend()
{
#if CLAP_WRAPPER_STANDALONE_RTMIDI
  return createRtMidiBackend();
#elif MAC
  return createCoreMidiBackend();
#elif WIN
  return createWinMmMidiBackend();
#elif CLAP_WRAPPER_STANDALONE_ALSASEQ
  return createAlsaSeqMidiBackend();
#else
  return nullptr;
#endif
}
}  // namespace freeaudio::clap_wrapper::standalone
