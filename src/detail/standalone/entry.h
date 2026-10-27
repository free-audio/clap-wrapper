#pragma once

#include <clap/clap.h>
#include <clap_proxy.h>
#include <string>
#include <vector>

namespace freeaudio::clap_wrapper::standalone
{
std::shared_ptr<Clap::Plugin> mainCreatePlugin(const clap_plugin_entry *entry, const std::string &clapId,
                                               uint32_t clapIndex, int argc, char **argv);
void mainStartAudio();

std::shared_ptr<Clap::Plugin> getMainPlugin();

struct StandaloneHost;
StandaloneHost *getStandaloneHost();

int mainWait();
int mainFinish();

// with --list-audio-devices among the arguments: prints every audio api's devices and the
// MIDI ports, as the standalone sees them, to stdout and returns true; the caller then
// exits. false otherwise. for support, and to compare backends on one machine
bool mainListAudioDevices(int argc, const char *const *argv);

// whether the standalone was built to open urls in any scheme (URL_SCHEMES)
bool opensUrls();

// whether url is in one of the schemes this standalone was built to open, which no url is
// in a standalone built with none
bool isOwnUrl(const std::string &url);

// the arguments that are such urls: on windows and linux, what a link clicked in a browser
// launches the standalone with
std::vector<std::string> ownUrlsInArgs(int argc, const char *const *argv);
}  // namespace freeaudio::clap_wrapper::standalone