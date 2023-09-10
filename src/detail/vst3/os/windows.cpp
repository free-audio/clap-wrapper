#define NOMINMAX 1

/**
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
#include "osutil.h"
#include <filesystem>

// from dllmain.cpp of the VST3 SDK
extern HINSTANCE ghInst;

namespace os
{

  void log(const char* text)
  {
    OutputDebugStringA(text);
    OutputDebugStringA("\n");
  }

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

  static Steinberg::ModuleInitializer createMessageWindow([] { gWindowsHelper.init(); });
  static Steinberg::ModuleTerminator dropMessageWindow([] { gWindowsHelper.terminate(); });

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

  std::string getParentFolderName()
  {
    std::filesystem::path n = getModuleNameA();
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
    std::filesystem::path n = getModuleNameA();
    if (n.has_filename())
    {
      return n.stem().u8string();
    }
    return std::string();
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
    wc.lpfnWndProc = (WNDPROC)&Wndproc;
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

  void WindowsHelper::executeDefered()
  {
    for (auto&& p : _plugs) p->onIdle();
  }

  void WindowsHelper::attach(IPlugObject* plugobject)
  {
    _plugs.push_back(plugobject);
  }

  void WindowsHelper::detach(IPlugObject* plugobject)
  {
    _plugs.erase(std::remove(_plugs.begin(), _plugs.end(), plugobject), _plugs.end());
  }

  // [UI Thread]
  void attach(IPlugObject* plugobject)
  {
    gWindowsHelper.attach(plugobject);
  }

  // [UI Thread]
  void detach(IPlugObject* plugobject)
  {
    gWindowsHelper.detach(plugobject);
  }

  uint64_t getTickInMS()
  {
    return GetTickCount64();
  }
}  // namespace os
