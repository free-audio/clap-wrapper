#pragma once

#include <string>

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
 * Ask for an orderly shutdown on SIGINT/SIGTERM/SIGHUP rather than dying where
 * we stand: the runloops poll quitRequested(), so ^C unwinds through the normal
 * path which stops audio, saves settings and destroys the plugin. A second
 * signal exits immediately, in case that path is itself wedged.
 */
void installSignalHandlers();
bool quitRequested();

/*
 * Idle until the standalone is asked to stop - either the host stopped running
 * or a signal arrived. For the case where there is no GUI runloop to sit in.
 */
void waitForQuit();
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
