#if CLAP_WRAPPER_HAS_WIN32

#include <string>
#include <Windows.h>

#include "../entry.h"
#include "../standalone_details.h"
#include "../../clap/fsutil.h"

#include "winutils.h"

namespace freeaudio::clap_wrapper::standalone::windows
{
// A very basic win32 window class
class Window
{
 public:
  Window();
  ~Window();

  HWND get_hwnd();

 private:
  HWND m_hwnd;
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
};

// Get window instance in our WndProc and set the m_hwnd member variable on creation
// also hide the console since we aren't a real Win32 application
template <class T, class U, HWND(U::*m_hwnd)>
T* InstanceFromWndProc(HWND hwnd, UINT umsg, LPARAM lparam)
{
  T* pInstance;

  if (umsg == WM_NCCREATE)
  {
    LPCREATESTRUCT pCreateStruct{reinterpret_cast<LPCREATESTRUCT>(lparam)};
    pInstance = reinterpret_cast<T*>(pCreateStruct->lpCreateParams);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pInstance));
    pInstance->*m_hwnd = hwnd;
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
  }

  else
    pInstance = reinterpret_cast<T*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  return pInstance;
}

std::string narrow(std::wstring in)
{
  if (!in.empty())
  {
    auto inSize{static_cast<int>(in.size())};

    auto outSize{::WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS | WC_ERR_INVALID_CHARS, in.data(),
                                       inSize, nullptr, 0, nullptr, nullptr)};

    if (outSize > 0)
    {
      std::string out;
      out.resize(static_cast<size_t>(outSize));

      if (::WideCharToMultiByte(CP_UTF8, WC_NO_BEST_FIT_CHARS | WC_ERR_INVALID_CHARS, in.data(), inSize,
                                out.data(), outSize, nullptr, nullptr) > 0)
        return out;
    }
  }

  return {};
}

std::wstring widen(std::string in)
{
  if (!in.empty())
  {
    auto inSize{static_cast<int>(in.size())};

    auto outSize{::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), inSize, nullptr, 0)};

    if (outSize > 0)
    {
      std::wstring out;
      out.resize(static_cast<size_t>(outSize));

      if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in.data(), inSize, out.data(), outSize) >
          0)
        return out;
    }
  }

  return {};
}

Window::Window()
{
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  std::wstring clapName{widen(OUTPUT_NAME)};

  WNDCLASSEXW wcex{sizeof(WNDCLASSEX)};
  wcex.lpszClassName = clapName.c_str();
  wcex.lpszMenuName = clapName.c_str();
  wcex.lpfnWndProc = Window::WndProc;
  wcex.style = CS_HREDRAW | CS_VREDRAW;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = ::GetModuleHandleW(nullptr);
  wcex.hbrBackground = (HBRUSH)::GetStockObject(BLACK_BRUSH);
  wcex.hCursor = (HCURSOR)::LoadImageW(nullptr, (LPCWSTR)IDC_ARROW, IMAGE_CURSOR, 0, 0, LR_SHARED);
  wcex.hIcon = (HICON)::LoadImageW(nullptr, (LPCWSTR)IDI_APPLICATION, IMAGE_ICON, 0, 0,
                                   LR_DEFAULTCOLOR | LR_DEFAULTSIZE | LR_SHARED);
  wcex.hIconSm = (HICON)::LoadImageW(nullptr, (LPCWSTR)IDI_APPLICATION, IMAGE_ICON, 0, 0,
                                     LR_DEFAULTCOLOR | LR_DEFAULTSIZE | LR_SHARED);

  auto atom{::RegisterClassExW(&wcex)};

  ::CreateWindowExW(0, clapName.c_str(), clapName.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                    ::GetModuleHandleW(nullptr), this);

  ::ShowWindow(m_hwnd, SW_SHOWDEFAULT);
}

Window::~Window()
{
}

HWND Window::get_hwnd()
{
  return m_hwnd;
}

LRESULT CALLBACK Window::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
  Window* window = InstanceFromWndProc<Window, Window, &Window::m_hwnd>(hwnd, msg, lparam);

  if (window)
  {
    switch (msg)
    {
      case WM_SIZE:
      {
        RECT r;
        ::GetClientRect(hwnd, &r);

        auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

        // Need to check if plugin is resizable, disable plugin gui resizing for now
        if (plugin && plugin->_ext._gui &&
            freeaudio::clap_wrapper::standalone::getStandaloneHost()->gui_can_resize())
          plugin->_ext._gui->set_size(plugin->_plugin, (r.right - r.left), (r.bottom - r.top));

        return 0;
      }

      case WM_CLOSE:
      {
        auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

        if (plugin && plugin->_ext._gui)
        {
          plugin->_ext._gui->hide(plugin->_plugin);
          plugin->_ext._gui->destroy(plugin->_plugin);
        }

        ::DestroyWindow(hwnd);

        return 0;
      }

      case WM_DESTROY:
      {
        freeaudio::clap_wrapper::standalone::mainFinish();

        ::PostQuitMessage(0);

        return 0;
      }

      case WM_DPICHANGED:
      {
        // Since scaling is not fully implemented, get plugin dimensions and size window to initial value

        auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

        uint32_t w;
        uint32_t h;
        plugin->_ext._gui->get_size(plugin->_plugin, &w, &h);

        RECT r{0};
        r.right = w;
        r.bottom = h;

        auto dpi{::GetDpiForWindow(hwnd)};

        auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};

        LOG << "dpi: " << dpi << std::endl;
        LOG << "scaleFactor: " << scaleFactor << std::endl;

        ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, 0, 0, dpi);
        ::SetWindowPos(hwnd, nullptr, 0, 0, (r.right - r.left), (r.bottom - r.top), SWP_NOMOVE);

        // Resize the window to suggested rectangle from system
        // auto suggestedRect{(RECT*)lparam};
        // ::SetWindowPos(hwnd, nullptr, suggestedRect->left, suggestedRect->top,
        //              (suggestedRect->right - suggestedRect->left),
        //              (suggestedRect->bottom - suggestedRect->top), SWP_NOZORDER | SWP_NOACTIVATE);

        return 0;
      }
    }
  }

  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void Win32Gui::initialize(freeaudio::clap_wrapper::standalone::StandaloneHost* sah)
{
  sah->win32Gui = this;
  sah->gui_can_resize();
}

void Win32Gui::setPlugin(std::shared_ptr<Clap::Plugin> p)
{
  plugin = p;
}

void Win32Gui::run()
{
  if (plugin->_ext._gui)
  {
    auto ui{plugin->_ext._gui};
    auto p{plugin->_plugin};

    if (!ui->is_api_supported(p, CLAP_WINDOW_API_WIN32, false)) LOG << "NO WIN32 " << std::endl;

    ui->create(p, CLAP_WINDOW_API_WIN32, false);
    ui->set_scale(p, 1);

    uint32_t w;
    uint32_t h;
    ui->get_size(p, &w, &h);

    Window window;
    RECT r{0};
    r.right = w;
    r.bottom = h;

    auto dpi{::GetDpiForWindow(window.get_hwnd())};
    auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};
    ::AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, 0, 0, dpi);
    ::SetWindowPos(window.get_hwnd(), nullptr, 0, 0, (r.right - r.left), (r.bottom - r.top), SWP_NOMOVE);

    clap_window win;
    win.api = CLAP_WINDOW_API_WIN32;
    win.win32 = (void*)window.get_hwnd();
    ui->set_parent(p, &win);
    ui->show(p);
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
