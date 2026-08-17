#include "linux_frontend.h"

#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <algorithm>

#include "detail/standalone/standalone_details.h"
#include "detail/standalone/standalone_host.h"
#include "detail/standalone/entry.h"

extern char **environ;

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
namespace
{
volatile sig_atomic_t quitFlag{0};

extern "C" void requestQuitHandler(int sig)
{
  if (quitFlag)
  {
    // We already asked nicely once and we're still here, so the orderly path is
    // evidently stuck. _exit is async-signal-safe; exit() is not.
    _exit(128 + sig);
  }
  quitFlag = 1;
}

bool isExecutable(const std::string &p)
{
  return !p.empty() && access(p.c_str(), X_OK) == 0;
}

std::string findInPath(const std::string &exe)
{
  auto path = getenv("PATH");
  if (!path) return {};

  std::string sp{path};
  size_t pos{0};
  while (pos <= sp.size())
  {
    auto next = sp.find(':', pos);
    auto dir = sp.substr(pos, (next == std::string::npos) ? std::string::npos : next - pos);
    if (!dir.empty())
    {
      auto cand = dir + "/" + exe;
      if (isExecutable(cand)) return cand;
    }
    if (next == std::string::npos) break;
    pos = next + 1;
  }
  return {};
}

bool hasDisplay()
{
  auto d = getenv("DISPLAY");
  auto w = getenv("WAYLAND_DISPLAY");
  return (d && *d) || (w && *w);
}

bool preferKDialog()
{
  auto d = getenv("XDG_CURRENT_DESKTOP");
  if (!d) return false;
  std::string s{d};
  return s.find("KDE") != std::string::npos || s.find("kde") != std::string::npos;
}

/*
 * The argv for whichever prompt tool this desktop has. Empty when it has
 * neither, which is a perfectly ordinary state of affairs - stderr still got
 * the message.
 */
std::vector<std::string> dialogCommand(const std::string &title, const std::string &message)
{
  static const auto zenity = findInPath("zenity");
  static const auto kdialog = findInPath("kdialog");

  auto useKDialog = preferKDialog() ? !kdialog.empty() : (zenity.empty() && !kdialog.empty());

  if (useKDialog)
  {
    return {kdialog, "--title", title, "--error", message};
  }
  if (!zenity.empty())
  {
    // --no-markup so a device name containing an ampersand produces a message
    // rather than a pango parse error
    return {zenity, "--error", "--no-markup", "--title=" + title, "--text=" + message};
  }
  return {};
}

/*
 * Rate limiting for the dialogs. RtAudio can report the same failure on every
 * callback, and a wall of modal prompts is worse than no prompt at all, so each
 * distinct message is shown once, only one dialog is up at a time, and we stop
 * after a handful. None of this gates stderr.
 */
struct DialogGate
{
  static constexpr int maxDialogs{3};

  std::mutex mutex;
  std::set<std::string> alreadyShown;
  int shown{0};
  bool inFlight{false};

  bool claim(const std::string &key)
  {
    std::lock_guard<std::mutex> g(mutex);
    if (inFlight || shown >= maxDialogs) return false;
    if (!alreadyShown.insert(key).second) return false;
    shown++;
    inFlight = true;
    return true;
  }

  void release()
  {
    std::lock_guard<std::mutex> g(mutex);
    inFlight = false;
  }
};

DialogGate &dialogGate()
{
  static DialogGate g;
  return g;
}

void runDialogDetached(std::vector<std::string> command)
{
  // The dialog outlives this call, so give it a thread of its own which reaps
  // the child rather than leaving a zombie and then lets the next error
  // through. posix_spawn (not fork) because we may well be on RtAudio's
  // stream thread.
  std::thread(
      [command = std::move(command)]()
      {
        std::vector<char *> argv;
        argv.reserve(command.size() + 1);
        for (auto &c : command) argv.push_back(const_cast<char *>(c.c_str()));
        argv.push_back(nullptr);

        pid_t pid{0};
        auto err = posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ);
        if (err != 0)
        {
          LOGINFO("[ERROR] Unable to spawn '{}' : {}", command[0], strerror(err));
        }
        else
        {
          int status{0};
          while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
          {
          }
        }
        dialogGate().release();
      })
      .detach();
}

/*
 * A backend with no output device is a backend whose server isn't running -
 * an unstarted JACK, or Pulse on a box with neither PulseAudio nor PipeWire.
 * The probe swallows errors: 'not available' is an answer, not a failure.
 */
bool apiHasOutputDevices(RtAudio::Api api)
{
  try
  {
    RtAudio probe(api, [](RtAudioErrorType, const std::string &) {});
    for (auto id : probe.getDeviceIds())
    {
      if (probe.getDeviceInfo(id).outputChannels > 0) return true;
    }
  }
  catch (...)
  {
  }
  return false;
}

std::string lowercased(const std::string &s)
{
  std::string r{s};
  std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return r;
}
}  // namespace

std::vector<RtAudio::Api> compiledAudioApis()
{
  std::vector<RtAudio::Api> res;
  RtAudio::getCompiledApi(res);
  return res;
}

RtAudio::Api resolveAudioApiName(const std::string &name)
{
  auto lower = lowercased(name);
  if (lower.empty() || lower == "auto" || lower == "default") return RtAudio::Api::UNSPECIFIED;

  // RtAudio 6 has no native PipeWire backend; its Pulse backend is how you get
  // at a PipeWire graph, and 'pipewire' is what a user will reasonably type
  if (lower == "pipewire" || lower == "pw") lower = "pulse";

  return RtAudio::getCompiledApiByName(lower);
}

void selectAudioApi(const std::string &requestedName)
{
  auto host = getStandaloneHost();

  if (!requestedName.empty() && lowercased(requestedName) != "auto")
  {
    auto api = resolveAudioApiName(requestedName);
    if (api == RtAudio::Api::UNSPECIFIED)
    {
      std::string available;
      for (auto a : compiledAudioApis())
      {
        if (!available.empty()) available += ", ";
        available += RtAudio::getApiName(a);
      }
      fprintf(stderr,
              "[ERROR] This build has no audio API called '%s'. Available: %s. Falling back to "
              "the default order.\n",
              requestedName.c_str(), available.c_str());
    }
    else
    {
      host->setAudioApi(api);
      LOGINFO("Audio API (requested) : {}", RtAudio::getApiDisplayName(api));
      fprintf(stderr, "[INFO] audio api: %s\n", RtAudio::getApiDisplayName(api).c_str());

      // Say this plainly here: what the user gets otherwise is RtAudio's
      // "deviceId argument not found" from somewhere deep in the open
      if (!apiHasOutputDevices(api))
      {
        fprintf(stderr,
                "[WARNING] The %s backend reports no output devices. If it needs a server "
                "running - JACK, PulseAudio, PipeWire - start it, or choose another with "
                "--audio-api.\n",
                RtAudio::getApiDisplayName(api).c_str());
      }
      return;
    }
  }

  auto compiled = compiledAudioApis();
  auto have = [&compiled](RtAudio::Api a)
  { return std::find(compiled.begin(), compiled.end(), a) != compiled.end(); };

  for (auto pref : {RtAudio::Api::LINUX_PULSE, RtAudio::Api::UNIX_JACK, RtAudio::Api::LINUX_ALSA,
                    RtAudio::Api::LINUX_OSS})
  {
    if (!have(pref)) continue;
    if (!apiHasOutputDevices(pref)) continue;

    host->setAudioApi(pref);
    LOGINFO("Audio API : {}", RtAudio::getApiDisplayName(pref));
    fprintf(stderr, "[INFO] audio api: %s\n", RtAudio::getApiDisplayName(pref).c_str());
    return;
  }

  // Nothing we prefer had a device. Leave the host unspecified and let RtAudio
  // make its own choice, so a backend we didn't think of still gets a chance.
  reportError("No audio backend",
              "None of the audio backends this build has - which is where PulseAudio, PipeWire, "
              "JACK and ALSA would appear - reported an output device. Letting RtAudio choose.");
}

void installAudioErrorReporter()
{
  getStandaloneHost()->displayAudioError = [](const std::string &msg)
  { reportError("Unable to configure audio", msg); };
}

void installSignalHandlers()
{
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = requestQuitHandler;
  sigemptyset(&sa.sa_mask);
  // Deliberately not SA_RESTART, so a blocking epoll_wait/read sees EINTR and
  // the runloop gets a chance to notice
  sa.sa_flags = 0;

  for (auto sig : {SIGINT, SIGTERM, SIGHUP})
  {
    if (sigaction(sig, &sa, nullptr) != 0)
    {
      LOGINFO("[ERROR] Unable to install handler for signal {} : {}", sig, strerror(errno));
    }
  }

  // A dead X11 or audio server socket should be an error we handle, not a death
  // sentence delivered mid-shutdown
  signal(SIGPIPE, SIG_IGN);
}

bool quitRequested()
{
  return quitFlag != 0;
}

void waitForQuit()
{
  auto sah = getStandaloneHost();
  while (sah->running && !quitRequested())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void reportError(const std::string &title, const std::string &message)
{
  LOGINFO("[ERROR] {} : {}", title, message);

  // LOGINFO is compiled out in release builds, and a standalone which fails to
  // start audio has to leave the user something, so stderr unconditionally.
  fprintf(stderr, "[ERROR] %s\n        %s\n", title.c_str(), message.c_str());
  fflush(stderr);

  if (!hasDisplay()) return;

  // Long messages make for unusable dialogs; stderr has the whole thing.
  constexpr size_t maxLen{1000};
  auto shortMessage = (message.size() > maxLen) ? message.substr(0, maxLen) + "..." : message;

  auto command = dialogCommand(title, shortMessage);
  if (command.empty()) return;

  if (!dialogGate().claim(title + "\n" + shortMessage)) return;

  runDialogDetached(std::move(command));
}
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
