#pragma once

#include <string>
#include <vector>

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"  // other peoples errors are outside my scope
#endif

#include "RtAudio.h"

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

/*
 * Bits of the Linux standalone which are not the X11 GUI: telling the user
 * something went wrong, and shutting down when the OS asks us to. These are
 * compiled whether or not the X11 GUI is enabled.
 */
namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
/*
 * Show the user an error. There is no toolkit here, so this always writes to
 * stderr and, if the session has one of the standard desktop prompt tools
 * (zenity or kdialog), also puts up a message box.
 *
 * Safe to call from any thread: RtAudio delivers its error callback on the
 * stream thread, and the dialog is run by a detached worker rather than by the
 * caller. Repeated messages are only dialogged once, and only a handful of
 * dialogs are ever shown, so a device which errors continuously can't bury the
 * desktop in prompts. stderr always gets everything.
 */
void reportError(const std::string &title, const std::string &message);

/*
 * Point the standalone host's audio error reporting at reportError. Without
 * this every RtAudio failure is silent and the app just runs with no sound.
 */
void installAudioErrorReporter();

/*
 * Ask for an orderly shutdown on SIGINT/SIGTERM/SIGHUP rather than dying where
 * we stand: the runloops poll quitRequested(), so ^C unwinds through the normal
 * path which stops audio, saves settings and destroys the plugin. A second
 * signal exits immediately, in case that path is itself wedged.
 */
void installSignalHandlers();
bool quitRequested();

/*
 * Choose the audio backend and hand it to the host.
 *
 * RtAudio's own probe order is ALSA, then JACK, then Pulse, first-non-empty
 * wins - and ALSA always has devices, so JACK and Pulse are never reached even
 * when they are compiled in. On a stock PipeWire desktop that means the
 * standalone talks to raw ALSA and never touches the PipeWire graph. So: prefer
 * Pulse (which is how you reach PipeWire - RtAudio 6 has no native PipeWire
 * backend), then JACK, then ALSA, taking the first which actually has an output
 * device.
 *
 * requestedName names a backend explicitly: 'alsa', 'pulse', 'jack',
 * 'pipewire' as an alias for pulse, or 'auto' for the order above. An unknown
 * one is reported and falls back to that order.
 */
void selectAudioApi(const std::string &requestedName = {});

// UNSPECIFIED for an empty/'auto' name, and also for one this build does not
// have
RtAudio::Api resolveAudioApiName(const std::string &name);

// the backends this build was compiled with, for help and error text
std::vector<RtAudio::Api> compiledAudioApis();

/*
 * Idle until the standalone is asked to stop - either the host stopped running
 * or a signal arrived. For the case where there is no GUI runloop to sit in.
 */
void waitForQuit();
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
