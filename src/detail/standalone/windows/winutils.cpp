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
      lib.load(clapPath);
      entry = lib._pluginEntry;
    }
  }
#endif

  if (!entry) return 0;

  std::string pid{PLUGIN_ID};
  int pindex{PLUGIN_INDEX};

  auto plugin{freeaudio::clap_wrapper::standalone::mainCreatePlugin(entry, pid, pindex, 1, __argv)};

  freeaudio::clap_wrapper::standalone::mainStartAudio();

  freeaudio::clap_wrapper::standalone::windows::Win32Gui win32Gui{};

  auto sah{freeaudio::clap_wrapper::standalone::getStandaloneHost()};

  sah->win32Gui = &win32Gui;

  win32Gui.plugin = plugin;

  if (plugin->_ext._gui)
  {
    auto ui{plugin->_ext._gui};
    auto p{plugin->_plugin};

    ui->create(p, CLAP_WINDOW_API_WIN32, false);

    freeaudio::clap_wrapper::standalone::windows::Window window;

    auto dpi{::GetDpiForWindow(window.m_hwnd)};
    auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};
    ui->set_scale(p, scaleFactor);

    if (ui->can_resize(p))
    {
      // We can check here if we had a previous size but we aren't saving state yet
    }

    uint32_t w{0};
    uint32_t h{0};
    ui->get_size(p, &w, &h);

    RECT r{0, 0, 0, 0};
    r.right = w;
    r.bottom = h;
    ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, 0, 0, dpi);
    ::SetWindowPos(window.m_hwnd, nullptr, 0, 0, (r.right - r.left), (r.bottom - r.top), SWP_NOMOVE);

    if (!ui->can_resize(p))
    {
      ::SetWindowLongPtr(window.m_hwnd, GWL_STYLE,
                         ::GetWindowLongPtr(window.m_hwnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW |
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    }

    clap_window win;
    win.api = CLAP_WINDOW_API_WIN32;
    win.win32 = (void*)window.m_hwnd;
    ui->set_parent(p, &win);

    ui->show(p);

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
