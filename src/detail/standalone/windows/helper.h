#if CLAP_WRAPPER_HAS_WIN32

#ifdef UNICODE
#undef UNICODE
#endif

#include <Windows.h>
#include <dwmapi.h>

#include <string>
#include <random>

#pragma comment(lib, "dwmapi")

#define IDM_SETTINGS 1001
#define IDM_SAVE_STATE 1002
#define IDM_LOAD_STATE 1003
#define IDM_RESET_STATE 1004

namespace freeaudio::clap_wrapper::standalone::windows
{
struct Window
{
  Window();
  ~Window();

  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  virtual int OnClose(HWND, UINT, WPARAM, LPARAM);
  virtual int OnDestroy(HWND, UINT, WPARAM, LPARAM);
  virtual int OnDpiChanged(HWND, UINT, WPARAM, LPARAM);
  virtual int OnKeyDown(HWND, UINT, WPARAM, LPARAM);
  virtual int OnWindowPosChanged(HWND, UINT, WPARAM, LPARAM);

  bool fullscreen();

  HWND m_hwnd;
  bool isConsoleAttached{false};
};

struct Console
{
  Console();
  ~Console();
  FILE* f;
};

template <class T, HWND(T::*m_hwnd)>
T* InstanceFromWndProc(HWND hwnd, UINT umsg, LPARAM lparam)
{
  T* pInstance;

  if (umsg == WM_NCCREATE)
  {
    LPCREATESTRUCT pCreateStruct{reinterpret_cast<LPCREATESTRUCT>(lparam)};
    pInstance = reinterpret_cast<T*>(pCreateStruct->lpCreateParams);
    ::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pInstance));
    pInstance->*m_hwnd = hwnd;
  }

  else
    pInstance = reinterpret_cast<T*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));

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

std::string randomize(std::string in)
{
  std::random_device rd;
  std::mt19937 mt(rd());
  std::uniform_real_distribution<double> dist(1.0, 10.0);
  auto randomDouble{dist(mt)};
  auto randomNumber{std::to_string(randomDouble)};
  randomNumber.erase(remove(randomNumber.begin(), randomNumber.end(), '.'), randomNumber.end());

  return (in + randomNumber);
}

Window::Window()
{
  std::string clapName{WIN32_NAME};
  std::string randomName{randomize(clapName)};

  WNDCLASSEX wcex{sizeof(WNDCLASSEX)};
  wcex.lpszClassName = randomName.c_str();
  wcex.lpszMenuName = randomName.c_str();
  wcex.lpfnWndProc = Window::WndProc;
  wcex.style = 0;
  wcex.cbClsExtra = 0;
  wcex.cbWndExtra = 0;
  wcex.hInstance = ::GetModuleHandle(nullptr);
  wcex.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
  wcex.hCursor = reinterpret_cast<HCURSOR>(::LoadImage(nullptr, reinterpret_cast<LPCSTR>(IDC_ARROW),
                                                       IMAGE_CURSOR, 0, 0, LR_SHARED | LR_DEFAULTSIZE));
  wcex.hIcon =
      reinterpret_cast<HICON>(::LoadImage(nullptr, reinterpret_cast<LPCSTR>(IDI_APPLICATION), IMAGE_ICON,
                                          0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE | LR_SHARED));
  wcex.hIconSm =
      reinterpret_cast<HICON>(::LoadImage(nullptr, reinterpret_cast<LPCSTR>(IDI_APPLICATION), IMAGE_ICON,
                                          0, 0, LR_DEFAULTCOLOR | LR_DEFAULTSIZE | LR_SHARED));

  ::RegisterClassEx(&wcex);

  ::CreateWindowEx(0, randomName.c_str(), clapName.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                   ::GetModuleHandle(nullptr), this);

  auto hMenu{::GetSystemMenu(m_hwnd, FALSE)};

  MENUITEMINFO seperator{sizeof(MENUITEMINFO)};
  seperator.fMask = MIIM_FTYPE;
  seperator.fType = MFT_SEPARATOR;

  MENUITEMINFO audioIn{sizeof(MENUITEMINFO)};
  audioIn.fMask = MIIM_STRING | MIIM_ID;
  audioIn.wID = IDM_SETTINGS;
  audioIn.dwTypeData = const_cast<LPSTR>("Settings");

  MENUITEMINFO saveState{sizeof(MENUITEMINFO)};
  saveState.fMask = MIIM_STRING | MIIM_ID;
  saveState.wID = IDM_SAVE_STATE;
  saveState.dwTypeData = const_cast<LPSTR>("Save state...");

  MENUITEMINFO loadState{sizeof(MENUITEMINFO)};
  loadState.fMask = MIIM_STRING | MIIM_ID;
  loadState.wID = IDM_LOAD_STATE;
  loadState.dwTypeData = const_cast<LPSTR>("Load state...");

  MENUITEMINFO resetState{sizeof(MENUITEMINFO)};
  resetState.fMask = MIIM_STRING | MIIM_ID;
  resetState.wID = IDM_RESET_STATE;
  resetState.dwTypeData = const_cast<LPSTR>("Reset state...");

  if (hMenu != INVALID_HANDLE_VALUE)
  {
    ::InsertMenuItem(hMenu, 1, TRUE, &seperator);
    ::InsertMenuItem(hMenu, 2, TRUE, &audioIn);
    ::InsertMenuItem(hMenu, 3, TRUE, &seperator);
    ::InsertMenuItem(hMenu, 4, TRUE, &saveState);
    ::InsertMenuItem(hMenu, 5, TRUE, &loadState);
    ::InsertMenuItem(hMenu, 6, TRUE, &resetState);
    ::InsertMenuItem(hMenu, 7, TRUE, &seperator);
  }
}

Window::~Window()
{
}

LRESULT CALLBACK Window::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  Window* pWindow = InstanceFromWndProc<Window, &Window::m_hwnd>(h, m, l);

  if (pWindow)
  {
    switch (m)
    {
      case WM_CLOSE:
        return pWindow->OnClose(h, m, w, l);
      case WM_DESTROY:
        return pWindow->OnDestroy(h, m, w, l);
      case WM_DPICHANGED:
        return pWindow->OnDpiChanged(h, m, w, l);
      case WM_KEYDOWN:
        return pWindow->OnKeyDown(h, m, w, l);
      case WM_WINDOWPOSCHANGED:
        return pWindow->OnWindowPosChanged(h, m, w, l);
    }
  }

  return ::DefWindowProc(h, m, w, l);
}

int Window::OnClose(HWND h, UINT m, WPARAM w, LPARAM l)
{
  ::DestroyWindow(h);

  return 0;
}

int Window::OnDestroy(HWND h, UINT m, WPARAM w, LPARAM l)
{
  auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

  if (plugin && plugin->getProxy()->canUseGui())
  {
    plugin->getProxy()->guiHide();
    plugin->getProxy()->guiDestroy();
  }

  ::PostQuitMessage(0);

  return 0;
}

int Window::OnDpiChanged(HWND h, UINT m, WPARAM w, LPARAM l)
{
  auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

  auto dpi{::GetDpiForWindow(h)};
  auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};

  plugin->getProxy()->guiSetScale(scaleFactor);

  auto bounds{(RECT*)l};
  ::SetWindowPos(h, nullptr, bounds->left, bounds->top, (bounds->right - bounds->left),
                 (bounds->bottom - bounds->top), SWP_NOZORDER | SWP_NOACTIVATE);

  return 0;
}

int Window::OnKeyDown(HWND h, UINT m, WPARAM w, LPARAM l)
{
  auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};

  switch (w)
  {
    case VK_F11:
    {
      if (plugin->getProxy()->guiCanResize()) fullscreen();

      break;
    }

    default:
      return 0;
  }

  return 0;
}

int Window::OnWindowPosChanged(HWND h, UINT m, WPARAM w, LPARAM l)
{
  auto plugin{freeaudio::clap_wrapper::standalone::getMainPlugin()};
  auto pluginProxy = plugin->getProxy();

  auto dpi{::GetDpiForWindow(h)};
  auto scaleFactor{static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI)};

  if (pluginProxy->guiCanResize())
  {
    RECT r{0, 0, 0, 0};
    ::GetClientRect(h, &r);
    uint32_t w = (r.right - r.left);
    uint32_t h = (r.bottom - r.top);
    pluginProxy->guiAdjustSize(&w, &h);
    pluginProxy->guiSetSize(w, h);
  }

#ifdef _DEBUG
  if (isConsoleAttached)
  {
    RECT wr{0, 0, 0, 0};
    ::GetWindowRect(h, &wr);
    ::SetWindowPos(::GetConsoleWindow(), nullptr, wr.left, wr.bottom, (wr.right - wr.left), 200,
                   SWP_NOZORDER | SWP_ASYNCWINDOWPOS);
  }
#endif

  return 0;
}

bool Window::fullscreen()
{
  static RECT pos;

  auto style{::GetWindowLongPtr(m_hwnd, GWL_STYLE)};

  if (style & WS_OVERLAPPEDWINDOW)
  {
    MONITORINFO mi = {sizeof(mi)};
    ::GetWindowRect(m_hwnd, &pos);
    if (::GetMonitorInfo(::MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
    {
      ::SetWindowLongPtr(m_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
      ::SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
    }

    return true;
  }

  else
  {
    ::SetWindowLongPtr(m_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    ::SetWindowPos(m_hwnd, nullptr, pos.left, pos.top, (pos.right - pos.left), (pos.bottom - pos.top),
                   SWP_FRAMECHANGED);

    return false;
  }
}

Console::Console()
{
  ::AllocConsole();
  ::EnableMenuItem(::GetSystemMenu(::GetConsoleWindow(), FALSE), SC_CLOSE,
                   MF_BYCOMMAND | MF_DISABLED | MF_GRAYED);
  ::freopen_s(&f, "CONOUT$", "w", stdout);
  ::freopen_s(&f, "CONOUT$", "w", stderr);
  ::freopen_s(&f, "CONIN$", "r", stdin);
  std::cout.clear();
  std::clog.clear();
  std::cerr.clear();
  std::cin.clear();
}

Console::~Console()
{
  ::fclose(f);
  ::FreeConsole();
}
}  // namespace freeaudio::clap_wrapper::standalone::windows

#endif
