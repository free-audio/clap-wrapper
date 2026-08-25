/*
    the Linux helper
    
    provides services for all plugin instances regarding Linux
    - global timer object
    - dispatch to UI thread
    - get binary name
*/

#include "public.sdk/source/main/moduleinit.h"
#include "osutil.h"
#include <filesystem>
#include <stdio.h>
#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <dlfcn.h>

namespace os
{
// A Linux VST3 plug-in has no main thread of its own to hang a timer on. The
// only main-thread callback a VST3 host offers here is Steinberg::Linux::
// IRunLoop, and that arrives through IPlugFrame -- which means it exists only
// while an editor is open. Everything onIdle() drives (clap_host::
// request_callback, clap timers, a deferred parameter flush, the queue of
// parameter edits bound for the host) was therefore dead whenever the plug-in
// window was shut.
//
// A thread of our own stands in for the main thread the host does not give us
// -- hence the _standIn members below. It covers that gap and only that gap: a
// plug object that has been handed a run loop reports hasOwnIdleSource() and is
// skipped, and when every attached object has one the thread parks on the
// condition variable rather than spinning. It is the same arrangement JUCE's
// Linux plug-in client uses, which runs a MessageThread of its own and stops it
// when the host's message thread appears.
constexpr auto idleInterval = std::chrono::milliseconds(10);

class LinuxHelper
{
 public:
  void init();
  void terminate();
  void attach(IPlugObject *plugobject);
  void detach(IPlugObject *plugobject);
  void idleSourceChanged();

 private:
  // both want _standInLock held
  bool anyoneWantsTicking() const;
  void executeDefered();

  void run();

  // Recursive, and paired with condition_variable_any, because it is held
  // across onIdle() and a plug object may come back through attach(), detach()
  // or idleSourceChanged() from inside its own idle.
  std::recursive_mutex _standInLock;
  std::condition_variable_any _standInWakeup;
  std::thread _standInThread;
  std::vector<IPlugObject *> _plugs;
  bool _standInRunning{false};
} gLinuxHelper;

#if 0
	class WindowsHelper
	{
	public:
		void init();
		void terminate();
		void attach(IPlugObject* plugobject);
		void detach(IPlugObject* plugobject);
	private:
		void executeDefered();
		static LRESULT Wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
		HWND _msgWin = 0;
		UINT_PTR _timer = 0;
		std::vector<IPlugObject*> _plugs;
	} gWindowsHelper;
#endif

static Steinberg::ModuleInitializer createMessageWindow([] { gLinuxHelper.init(); });
static Steinberg::ModuleTerminator dropMessageWindow([] { gLinuxHelper.terminate(); });

#if 0
	static char* getModuleNameA()
	{
		static char modulename[2048];
		HMODULE selfmodule;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)getModuleNameA, &selfmodule))
		{
			auto size = GetModuleFileNameA(selfmodule, modulename, 2048);
		}
		return modulename;
	}

	static TCHAR* getModuleName()
	{
		static TCHAR modulename[2048];
		HMODULE selfmodule;
		if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCTSTR)getModuleName, &selfmodule))
		{
			auto size = GetModuleFileName(selfmodule, modulename, 2048);
		}
		return modulename;
	}
#endif

fs::path getPluginPath()
{
  Dl_info info;
  if (dladdr((void *)getPluginPath, &info))
  {
    return info.dli_fname;
  }
  return {};
}

std::string getParentFolderName()
{
  fs::path n = getPluginPath();
  if (n.has_parent_path())
  {
    auto p = n.parent_path();
    if (p.has_filename())
    {
      return p.filename().u8string();
    }
  }

  return {};
}

std::string getBinaryName()
{
  fs::path n = getPluginPath();
  if (n.has_filename())
  {
    return n.stem().u8string();
  }
  return {};
}

#if 0
	LRESULT WindowsHelper::Wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_USER + 1:
			return 1;
			break;
		case WM_TIMER:
			gWindowsHelper.executeDefered();
			return 1;
			break;
		default:
			return ::DefWindowProc(hwnd, msg, wParam, lParam);
		}
	}

	void WindowsHelper::init()
	{
		auto modulename = getModuleName();
		WNDCLASSEX wc;
		memset(&wc, 0, sizeof(wc));
		wc.cbSize = sizeof(wc);
		wc.hInstance = ghInst;
		wc.lpfnWndProc = &Wndproc;
		wc.lpszClassName = modulename;
		auto a = RegisterClassEx(&wc);

		_msgWin = ::CreateWindowEx(0, modulename, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
		::SetWindowLongW(_msgWin, GWLP_WNDPROC, (LONG_PTR)&Wndproc);
		_timer = ::SetTimer(_msgWin, 0, 20, NULL);
	}

	void WindowsHelper::terminate()
	{
		::KillTimer(_msgWin, _timer);
		::DestroyWindow(_msgWin);
		::UnregisterClass(getModuleName(), ghInst);
	}

#endif
void LinuxHelper::init()
{
  // The thread starts with the first attach() rather than here: a host scanning
  // plug-ins dlopens and dlcloses this module without ever activating anything.
}

void LinuxHelper::terminate()
{
  {
    std::lock_guard<std::recursive_mutex> guard(_standInLock);
    _standInRunning = false;
  }
  _standInWakeup.notify_all();
  if (_standInThread.joinable()) _standInThread.join();
}

bool LinuxHelper::anyoneWantsTicking() const
{
  for (auto const *p : _plugs)
  {
    if (p && !p->hasOwnIdleSource()) return true;
  }
  return false;
}

void LinuxHelper::executeDefered()
{
  for (auto *p : _plugs)
  {
    // An object with a run loop is already being idled on the host's own main
    // thread; ticking it here too would give it two main threads.
    if (p && !p->hasOwnIdleSource()) p->onIdle();
  }
}

void LinuxHelper::run()
{
  LOGDETAIL("clap-wrapper: Linux idle thread started");

  std::unique_lock<std::recursive_mutex> guard(_standInLock);
  while (_standInRunning)
  {
    if (!anyoneWantsTicking())
    {
      LOGDETAIL("clap-wrapper: every attached object has a run loop, pausing the idle thread");
      _standInWakeup.wait(guard, [this] { return !_standInRunning || anyoneWantsTicking(); });
      continue;
    }

    _standInWakeup.wait_for(guard, idleInterval, [this] { return !_standInRunning; });
    if (!_standInRunning) break;

    // The lock is held across the idle deliberately: detach() runs on the
    // host's thread and is followed by the object's destruction, so it has to
    // be able to wait out an idle that is already in flight. Lock order is
    // helper-then-plug-object; nothing may call attach(), detach() or
    // idleSourceChanged() while holding a plug object's own lock.
    executeDefered();
  }

  LOGDETAIL("clap-wrapper: Linux idle thread exiting");
}

void LinuxHelper::attach(IPlugObject *plugobject)
{
  std::lock_guard<std::recursive_mutex> guard(_standInLock);
  _plugs.push_back(plugobject);

  if (!_standInRunning)
  {
    _standInRunning = true;
    _standInThread = std::thread([this] { run(); });
  }
  _standInWakeup.notify_all();
}

void LinuxHelper::detach(IPlugObject *plugobject)
{
  std::lock_guard<std::recursive_mutex> guard(_standInLock);
  _plugs.erase(std::remove(_plugs.begin(), _plugs.end(), plugobject), _plugs.end());
  _standInWakeup.notify_all();
}

void LinuxHelper::idleSourceChanged()
{
  _standInWakeup.notify_all();
}

}  // namespace os

namespace os
{
// [UI Thread]
void attach(IPlugObject *plugobject)
{
  gLinuxHelper.attach(plugobject);
}

// [UI Thread]
void detach(IPlugObject *plugobject)
{
  gLinuxHelper.detach(plugobject);
}

void idleSourceChanged()
{
  gLinuxHelper.idleSourceChanged();
}

uint64_t getTickInMS()
{
  // Wall-clock milliseconds since some fixed point — the CLAP timer extension
  // schedules in real time. Deliberately not clock(): that is process CPU time
  // summed over all threads, so it crawls while the process is idle (timers
  // never fire) and can outrun real time while the audio threads are busy.
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}
}  // namespace os
