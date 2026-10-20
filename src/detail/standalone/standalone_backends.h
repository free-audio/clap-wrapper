#pragma once

// the backends each platform and build provides, gathered by audioApis(),
// createAudioBackend() and createMidiBackend() in standalone_backends.cpp. a build with
// CLAP_WRAPPER_STANDALONE_RTAUDIO takes RtAudio for audio and, with
// CLAP_WRAPPER_STANDALONE_RTMIDI, RtMidi for MIDI; without it, macOS and Windows use their
// own apis, and Windows built with ASIO (CLAP_WRAPPER_STANDALONE_ASIO) adds RtAudio's ASIO
// next to WASAPI. Linux has PipeWire (CLAP_WRAPPER_STANDALONE_PIPEWIRE) and JACK
// (CLAP_WRAPPER_STANDALONE_JACK) for audio, and the ALSA sequencer
// (CLAP_WRAPPER_STANDALONE_ALSASEQ) for MIDI

#include "standalone_audio.h"
#include "standalone_midi.h"

namespace freeaudio::clap_wrapper::standalone
{
#if CLAP_WRAPPER_STANDALONE_RTAUDIO || CLAP_WRAPPER_STANDALONE_ASIO
// rtaudio_backend.cpp: the apis RtAudio was compiled with, in its order of preference, and
// a backend on one of them. with only ASIO from RtAudio, that is the one api
std::vector<AudioApi> rtAudioApis();
std::unique_ptr<AudioBackend> createRtAudioBackend(const std::string &apiId);
#endif

#if MAC
// macos/coreaudio_backend.cpp: the devices as CoreAudio lists them, which the CoreAudio
// backend shows and the RtAudio one takes what RtAudio cannot tell from
std::vector<AudioDevice> listCoreAudioDevices();
#endif

#if CLAP_WRAPPER_STANDALONE_RTMIDI
// rtmidi_backend.cpp
std::unique_ptr<MidiBackend> createRtMidiBackend();
#endif

#if !CLAP_WRAPPER_STANDALONE_RTAUDIO && MAC
// macos/coreaudio_backend.cpp, macos/coremidi_backend.cpp
std::unique_ptr<AudioBackend> createCoreAudioBackend();
std::unique_ptr<MidiBackend> createCoreMidiBackend();
#elif !CLAP_WRAPPER_STANDALONE_RTAUDIO && WIN
// windows/wasapi_backend.cpp, windows/winmm_midi_backend.cpp
std::unique_ptr<AudioBackend> createWasapiBackend();
std::unique_ptr<MidiBackend> createWinMmMidiBackend();
#endif

#if CLAP_WRAPPER_STANDALONE_PIPEWIRE
// linux/pipewire_backend.cpp. usable: the library loads and a server answers
std::unique_ptr<AudioBackend> createPipeWireBackend();
bool pipeWireUsable();
#endif
#if CLAP_WRAPPER_STANDALONE_JACK
// linux/jack_backend.cpp. usable: the library loads and a server answers
std::unique_ptr<AudioBackend> createJackBackend();
bool jackUsable();
#endif
#if CLAP_WRAPPER_STANDALONE_ALSASEQ
// linux/alsaseq_midi_backend.cpp
std::unique_ptr<MidiBackend> createAlsaSeqMidiBackend();
#endif
}  // namespace freeaudio::clap_wrapper::standalone
