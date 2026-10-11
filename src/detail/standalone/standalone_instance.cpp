#include "standalone_instance.h"
#include "standalone_details.h"

#include <string>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace freeaudio::clap_wrapper::standalone
{
InstanceSlot::~InstanceSlot()
{
#if defined(_WIN32)
  if (handle) ::CloseHandle(static_cast<HANDLE>(handle));
#else
  if (fd >= 0) ::close(fd);
#endif
}

uint32_t InstanceSlot::claim(const fs::path &directory)
{
  if (claimed) return index;
  claimed = true;

  for (uint32_t slot = 0; slot < maxSlots; ++slot)
  {
    const auto path{directory / ("instance-" + std::to_string(slot) + ".lock")};

#if defined(_WIN32)
    // Opened with no sharing at all: any other process opening the same file fails
    // with a sharing violation for as long as this handle stays open.
    auto h{::CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (h != INVALID_HANDLE_VALUE)
    {
      handle = h;
      index = slot;
      return index;
    }

    // Anything but "another instance has it" means the directory is unusable, and
    // trying the next slot would only fail the same way.
    if (::GetLastError() != ERROR_SHARING_VIOLATION) break;
#else
    auto f{::open(path.u8string().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644)};
    if (f < 0) break;

    if (::flock(f, LOCK_EX | LOCK_NB) == 0)
    {
      fd = f;
      index = slot;
      return index;
    }

    const auto error{errno};
    ::close(f);
    if (error != EWOULDBLOCK) break;
#endif
  }

  LOGINFO("[WARNING] Unable to claim a standalone instance slot in '{}'; using the first",
          directory.u8string());
  index = 0;
  return index;
}
}  // namespace freeaudio::clap_wrapper::standalone
