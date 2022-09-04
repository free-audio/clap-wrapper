#define NOMINMAX 1

/*
*		the windows helper
* 
*		provides services for all plugin instances regarding Windows
*		- global timer object
*		- dispatch to UI thread
* 
*/

#include <Windows.h>
#include <tchar.h>
#include "public.sdk/source/main/moduleinit.h"

// from dllmain.cpp of the VST3 SDK
extern HINSTANCE ghInst;

namespace ClapAsVst3Detail
{

	class WindowsHelper
	{
	public:
		void init();
		void terminate();
	private:
		void executeDefered();
		static LRESULT Wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
		HWND _msgWin = 0;
		UINT_PTR _timer = 0;
	} gWindowsHelper;

	static Steinberg::ModuleInitializer createMessageWindow([] { gWindowsHelper.init(); });
	static Steinberg::ModuleTerminator dropMessageWindow([] { gWindowsHelper.terminate(); });

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
		_timer = ::SetTimer(_msgWin, 0, 10, NULL);
	}

	void WindowsHelper::terminate()
	{
		::KillTimer(_msgWin, _timer);
		::DestroyWindow(_msgWin);
		::UnregisterClass(getModuleName(), ghInst);
	}

	void WindowsHelper::executeDefered()
	{
		// TODO: execute all things to be scheduled in the UI thread
	}
}