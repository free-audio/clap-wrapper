#if CLAP_WRAPPER_HAS_WIN32

#ifdef UNICODE
#undef UNICODE
#endif

#include <Windows.h>

#include "../entry.h"
#include "../standalone_details.h"
#include "../../clap/fsutil.h"

#include "winutils.h"
#include "helper.h"

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR pCmdLine, int nCmdShow)
{
#ifdef _DEBUG
  freeaudio::clap_wrapper::standalone::windows::Console console;
#endif

  const clap_plugin_entry* entry{nullptr};

#ifdef STATICALLY_LINKED_CLAP_ENTRY
  extern const clap_plugin_entry clap_entry;
  entry = &clap_entry;
#else
  std::string clapName{HOSTED_CLAP_NAME};

  auto searchPaths{Clap::getValidCLAPSearchPaths()};

  auto lib{Clap::Library()};

  for (const auto& searchPath : searchPaths)
  {
    auto clapPath = searchPath / (clapName + ".clap");

    if (fs::exists(clapPath) && !entry)
    {
      lib.load(clapPath.u8string().c_str());
      entry = lib._pluginEntry;
    }
  }
#endif

  if (!entry) return 0;

  std::string pid{PLUGIN_ID};
  int pindex{PLUGIN_INDEX};

  auto plugin{freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, __argv)};
  const auto proxy = plugin->getProxy();

  freeaudio::clap_wrapper::standalone::mainStartAudio();

  freeaudio::clap_wrapper::standalone::windows::Win32Gui win32Gui{};

  auto sah{freeaudio::clap_wrapper::standalone::getStandaloneHost()};

  sah->win32Gui = &win32Gui;

  win32Gui.plugin = plugin;

  if (proxy->canUseGui())
  {
    proxy->guiCreate(CLAP_WINDOW_API_WIN32, false);

    freeaudio::clap_wrapper::standalone::windows::Window window;

    auto dpi{::GetDpiForWindow(window.m_hwnd)};
    auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};
    proxy->guiSetScale(scaleFactor);

    if (proxy->guiCanResize())
    {
      // We can check here if we had a previous size but we aren't saving state yet
    }

    uint32_t w{0};
    uint32_t h{0};
    proxy->guiGetSize(&w, &h);

    RECT r{0, 0, 0, 0};
    r.right = w;
    r.bottom = h;
    ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, 0, 0, dpi);
    ::SetWindowPos(window.m_hwnd, nullptr, 0, 0, (r.right - r.left), (r.bottom - r.top), SWP_NOMOVE);

    if (!proxy->guiCanResize())
    {
      ::SetWindowLongPtr(window.m_hwnd, GWL_STYLE,
                         ::GetWindowLongPtr(window.m_hwnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW |
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    }

    clap_window win;
    win.api = CLAP_WINDOW_API_WIN32;
    win.win32 = (void*)window.m_hwnd;
    proxy->guiSetParent(&win);

    proxy->guiShow();

    ::ShowWindow(window.m_hwnd, SW_SHOWDEFAULT);
  }

  MSG msg{};
  int r{0};

  while ((r = ::GetMessage(&msg, nullptr, 0, 0)) != 0)
  {
    if (r == -1)
      return 0;

    else
    {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
    }
  }

  win32Gui.plugin = nullptr;

  plugin = nullptr;

  freeaudio::clap_wrapper::standalone::mainFinish();

  return 0;
}

#endif
