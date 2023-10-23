#if CLAP_WRAPPER_HAS_WIN32

#include <string>
#include <Windows.h>

#include "../entry.h"
#include "../standalone_details.h"
#include "../../clap/fsutil.h"

#include "winutils.h"
#include "helper.h"

namespace freeaudio::clap_wrapper::standalone::windows
{
struct Window
{
  Window();
  ~Window();

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  virtual int _OnClose(HWND, UINT, WPARAM, LPARAM);
  virtual int _OnDestroy(HWND, UINT, WPARAM, LPARAM);
  virtual int _OnDpiChanged(HWND, UINT, WPARAM, LPARAM);
  virtual int _OnKeyDown(HWND, UINT, WPARAM, LPARAM);
  virtual int _OnSize(HWND, UINT, WPARAM, LPARAM);

  HWND m_hwnd;
  bool fullscreen;
};

Window::Window()
{
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  std::wstring clapName{helper::widen(WIN32_TITLE)};
  std::wstring randomName{helper::randomize(clapName)};

  WNDCLASSEXW wcex{sizeof(WNDCLASSEX)};
  wcex.lpszClassName = randomName.c_str();
  wcex.lpszMenuName = randomName.c_str();
  wcex.lpfnWndProc = Window::WndProc;
  wcex.style = 0;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = ::GetModuleHandleW(nullptr);
  wcex.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
  wcex.hCursor = (HCURSOR)::LoadImageW(nullptr, (LPCWSTR)IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_SHARED);
  wcex.hIcon = (HICON)::LoadImageW(nullptr, (LPCWSTR)IDI_APPLICATION, IMAGE_ICON, 0, 0,
                                   LR_DEFAULTCOLOR | LR_DEFAULTSIZE | LR_SHARED);
  wcex.hIconSm = (HICON)::LoadImageW(nullptr, (LPCWSTR)IDI_APPLICATION, IMAGE_ICON, 0, 0,
                                     LR_DEFAULTCOLOR | LR_DEFAULTSIZE | LR_SHARED);

  ::RegisterClassExW(&wcex);

  ::CreateWindowExW(0, randomName.c_str(), clapName.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                    ::GetModuleHandleW(nullptr), this);

  ::ShowWindow(m_hwnd, SW_SHOWDEFAULT);
}

Window::~Window()
{
}

LRESULT CALLBACK Window::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  Window* pWindow = helper::InstanceFromWndProc<Window, &Window::m_hwnd>(hWnd, uMsg, lParam);

  if (pWindow)
  {
    switch (uMsg)
    {
      case WM_CLOSE:
        return pWindow->_OnClose(hWnd, uMsg, wParam, lParam);
      case WM_DESTROY:
        return pWindow->_OnDestroy(hWnd, uMsg, wParam, lParam);
      case WM_DPICHANGED:
        return pWindow->_OnDpiChanged(hWnd, uMsg, wParam, lParam);
      case WM_KEYDOWN:
        return pWindow->_OnKeyDown(hWnd, uMsg, wParam, lParam);
      case WM_SIZE:
        return pWindow->_OnSize(hWnd, uMsg, wParam, lParam);
    }
  }

  return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

int Window::_OnClose(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

  if (plugin && plugin->_ext._gui)
  {
    plugin->_ext._gui->hide(plugin->_plugin);
    plugin->_ext._gui->destroy(plugin->_plugin);
  }

  ::DestroyWindow(m_hwnd);

  return 0;
}

int Window::_OnDestroy(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  freeaudio::clap_wrapper::standalone::mainFinish();
  ::PostQuitMessage(0);

  return 0;
}

int Window::_OnDpiChanged(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  // auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

  // uint32_t w;
  // uint32_t h;
  // plugin->_ext._gui->get_size(plugin->_plugin, &w, &h);

  // RECT r{0, 0, 0, 0};
  // r.right = w;
  // r.bottom = h;

  // auto dpi{::GetDpiForWindow(hWnd)};

  // auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};

  // LOG << "dpi: " << dpi << std::endl;
  // LOG << "scaleFactor: " << scaleFactor << std::endl;

  // ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, 0, 0, dpi);
  // ::SetWindowPos(hWnd, nullptr, 0, 0, (r.right - r.left), (r.bottom - r.top), SWP_NOMOVE);

  // Resize the window to suggested rectangle from system
  // auto suggestedRect{(RECT*)lparam};
  // ::SetWindowPos(hWnd, nullptr, suggestedRect->left, suggestedRect->top,
  //              (suggestedRect->right - suggestedRect->left),
  //              (suggestedRect->bottom - suggestedRect->top), SWP_NOZORDER | SWP_NOACTIVATE);

  return 0;
}

int Window::_OnKeyDown(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (wParam)
  {
    case VK_F11:
    {
      fullscreen = helper::fullscreen(m_hwnd);

      break;
    }

    default:
      return 0;
  }

  return 0;
}

int Window::_OnSize(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  // auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

  // if (plugin && plugin->_ext._gui)
  // {
  //   RECT r{0, 0, 0, 0};
  //   ::GetClientRect(hWnd, &r);
  //   plugin->_ext._gui->set_size(plugin->_plugin, (r.right - r.left), (r.bottom - r.top));
  // }

  return 0;
}

void Win32Gui::initialize(freeaudio::clap_wrapper::standalone::StandaloneHost* sah)
{
  sah->win32Gui = this;
}

void Win32Gui::setPlugin(std::shared_ptr<Clap::Plugin> p)
{
  plugin = p;
}

void Win32Gui::run()
{
  if (plugin->_ext._gui)
  {
    Window window;
    RECT r{0, 0, 0, 0};

    auto dpi{::GetDpiForWindow(window.m_hwnd)};
    auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};

    auto ui{plugin->_ext._gui};
    auto p{plugin->_plugin};

    if (!ui->is_api_supported(p, CLAP_WINDOW_API_WIN32, false)) LOG << "NO WIN32 " << std::endl;

    ui->create(p, CLAP_WINDOW_API_WIN32, false);
    ui->set_scale(p, scaleFactor);

    uint32_t w;
    uint32_t h;
    ui->get_size(p, &w, &h);

    ui->set_size(p, (w * scaleFactor), (h * scaleFactor));

    clap_window win;
    win.api = CLAP_WINDOW_API_WIN32;
    win.win32 = (void*)window.m_hwnd;
    ui->set_parent(p, &win);
    ui->show(p);

    r.right = w * scaleFactor;
    r.bottom = h * scaleFactor;

    ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, 0, 0, dpi);
    ::SetWindowPos(window.m_hwnd, nullptr, 0, 0, (r.right - r.left), (r.bottom - r.top), SWP_NOMOVE);
  }

  MSG msg;
  int r;

  while ((r = ::GetMessageW(&msg, nullptr, 0, 0)) != 0)
  {
    if (r == -1)
      return;

    else
    {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }
}
}  // namespace freeaudio::clap_wrapper::standalone::windows

#endif
