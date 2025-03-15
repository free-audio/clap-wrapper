#define NOMINMAX 1

/*
    the windows helper
    
    provides services for all plugin instances regarding Windows
    - global timer object
    - dispatch to UI thread
 
*/

#include <windows.h>
#include <tchar.h>
#include "public.sdk/source/main/moduleinit.h"
#include "osutil.h"

// from dllmain.cpp of the VST3 SDK
extern HINSTANCE ghInst;

namespace os
{

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

fs::path getPluginPath()
{
  fs::path modulePath{};
  HMODULE selfmodule;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          (LPCWSTR)getPluginPath, &selfmodule))
  {
    std::wstring p;
    auto size = GetModuleFileNameW(selfmodule, nullptr, 0);
    if (size == 0)
    {
      LOGINFO("[ERROR] GetModuleFileNameW failed, win32 code: {}", GetLastError());
      return {};
    }
    p.resize(size);
    size = GetModuleFileNameW(selfmodule, &p[0], size);
    modulePath = std::move(p);
  }
  else
  {
    LOGINFO("[ERROR] GetModuleHandleExW failed, win32 code: {}", GetLastError());
    return {};
  }
  return modulePath;
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

  return std::string();
}

std::string getBinaryName()
{
  fs::path n = getPluginPath();
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
      return DefWindowProc(hwnd, msg, wParam, lParam);
  }
}

void WindowsHelper::init()
{
  auto modulename = getPluginPath();
  WNDCLASSEXW wc;
  memset(&wc, 0, sizeof(wc));
  wc.cbSize = sizeof(wc);
  wc.hInstance = ghInst;
  wc.lpfnWndProc = (WNDPROC)&Wndproc;
  wc.lpszClassName = modulename.c_str();
  /* auto a = */
  RegisterClassExW(&wc);

  _msgWin = CreateWindowExW(0, modulename.c_str(), NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0);
  SetWindowLongW(_msgWin, GWLP_WNDPROC, (LONG_PTR)&Wndproc);
  _timer = SetTimer(_msgWin, 0, 20, NULL);
}

void WindowsHelper::terminate()
{
  KillTimer(_msgWin, _timer);
  DestroyWindow(_msgWin);
  UnregisterClassW(getPluginPath().c_str(), ghInst);
}

void WindowsHelper::executeDefered()
{
  for (auto&& p : _plugs)
  {
    if (p) p->onIdle();
  }
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
