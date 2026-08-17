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
 * one is reported and falls back to that order. With no request, a backend named
 * in the persisted settings is used before the order above is consulted.
 *
 * The chosen name is written into the host's settings, which is where the shared
 * applyAudioSettings() reads the API from.
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

/*
 * Start a detached timer which ends the process outright if the teardown which
 * follows has not finished within seconds.
 *
 * This exists because of an RtAudio/ALSA deadlock we cannot reach from here:
 * RtApiAlsa::callbackEvent() takes the stream mutex and holds it across the
 * blocking snd_pcm_readi() of a duplex stream, while RtApiAlsa::stopStream()
 * wants that same mutex. If the capture side has stopped producing - which a
 * PipeWire or dmix capture device does readily - the read never returns, the
 * mutex is never released and stopping the stream blocks for ever. Both ^C and
 * closing the window then leave a process which has to be killed.
 *
 * So: give the orderly shutdown a fixed budget and take the exit if it overruns.
 * shutdownFinished() cancels the timer.
 */
void armShutdownWatchdog(int seconds);
void shutdownFinished();
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
