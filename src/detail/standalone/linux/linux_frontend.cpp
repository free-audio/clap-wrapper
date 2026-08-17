#include "linux_frontend.h"

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "detail/standalone/standalone_details.h"

extern char **environ;

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
namespace
{
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
}  // namespace

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
