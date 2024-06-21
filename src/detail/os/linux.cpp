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

#include <dlfcn.h>

namespace os
{
void log(const char* text)
{
  fprintf(stderr, "%s\n", text);
}

class LinuxHelper
{
 public:
  void init();
  void terminate();
  void attach(IPlugObject* plugobject);
  void detach(IPlugObject* plugobject);

 private:
  void executeDefered();
  std::vector<IPlugObject*> _plugs;
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

std::string getModulePath()
{
  Dl_info info;
  if (dladdr((void*)getModulePath, &info))
  {
    return info.dli_fname;
  }
  return std::string();
}

std::string getParentFolderName()
{
  std::filesystem::path n = getModulePath();
  if (n.has_parent_path())
  {
    auto p = n.parent_path();
    if (p.has_filename())
    {
      return p.filename().u8string();
    }
  }

  return std::string();
}

std::string getBinaryName()
{
  std::filesystem::path n = getModulePath();
  if (n.has_filename())
  {
    return n.stem().u8string();
  }
  return std::string();
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
}

void LinuxHelper::terminate()
{
}

void LinuxHelper::executeDefered()
{
  for (auto p : _plugs)
  {
    if (p) p->onIdle();
  }
}
void LinuxHelper::attach(IPlugObject* plugobject)
{
  _plugs.push_back(plugobject);
}

void LinuxHelper::detach(IPlugObject* plugobject)
{
  _plugs.erase(std::remove(_plugs.begin(), _plugs.end(), plugobject), _plugs.end());
}

}  // namespace os

namespace os
{
// [UI Thread]
void attach(IPlugObject* plugobject)
{
  gLinuxHelper.attach(plugobject);
}

// [UI Thread]
void detach(IPlugObject* plugobject)
{
  gLinuxHelper.detach(plugobject);
}

uint64_t getTickInMS()
{
  return clock();
}
}  // namespace os
