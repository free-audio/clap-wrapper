#include "standalone_backends.h"
#include "standalone_details.h"

#include <algorithm>

namespace freeaudio::clap_wrapper::standalone
{
namespace
{
// the native api, which comes first and is the default
#if !CLAP_WRAPPER_STANDALONE_RTAUDIO && MAC
const AudioApi nativeApi{"core", "CoreAudio"};
#elif !CLAP_WRAPPER_STANDALONE_RTAUDIO && WIN
const AudioApi nativeApi{"wasapi", "WASAPI"};
#endif
}  // namespace

std::vector<AudioApi> audioApis()
{
  std::vector<AudioApi> res;
#if !CLAP_WRAPPER_STANDALONE_RTAUDIO && (MAC || WIN)
  res.push_back(nativeApi);
#endif
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

#if !CLAP_WRAPPER_STANDALONE_RTAUDIO && MAC
  if (known->id == nativeApi.id) return createCoreAudioBackend();
#elif !CLAP_WRAPPER_STANDALONE_RTAUDIO && WIN
  if (known->id == nativeApi.id) return createWasapiBackend();
#endif
#if CLAP_WRAPPER_STANDALONE_RTAUDIO || CLAP_WRAPPER_STANDALONE_ASIO
  return createRtAudioBackend(known->id);
#else
  return nullptr;
#endif
}

std::unique_ptr<MidiBackend> createMidiBackend()
{
#if CLAP_WRAPPER_STANDALONE_RTAUDIO
  return createRtMidiBackend();
#elif MAC
  return createCoreMidiBackend();
#elif WIN
  return createWinMmMidiBackend();
#else
  return nullptr;
#endif
}
}  // namespace freeaudio::clap_wrapper::standalone
