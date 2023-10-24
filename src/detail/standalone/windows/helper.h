#if CLAP_WRAPPER_HAS_WIN32

#include <Windows.h>

#include <string>
#include <random>

namespace freeaudio::clap_wrapper::standalone::windows::helper
{
template <class T, HWND(T::*m_hwnd)>
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

std::wstring randomize(std::wstring in)
{
  std::random_device rd;
  std::mt19937 mt(rd());
  std::uniform_real_distribution<double> dist(1.0, 10.0);
  auto randomDouble{dist(mt)};
  auto randomNumber{std::to_wstring(randomDouble)};
  randomNumber.erase(remove(randomNumber.begin(), randomNumber.end(), '.'), randomNumber.end());

  return (in + randomNumber);
}

bool fullscreen(HWND hwnd)
{
  static RECT pos;

  auto style{GetWindowLongPtrW(hwnd, GWL_STYLE)};

  if (style & WS_OVERLAPPEDWINDOW)
  {
    MONITORINFO mi = {sizeof(mi)};
    GetWindowRect(hwnd, &pos);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
    {
      SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
      SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                   mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                   SWP_FRAMECHANGED);
    }

    return true;
  }

  else
  {
    SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPos(hwnd, nullptr, pos.left, pos.top, (pos.right - pos.left), (pos.bottom - pos.top),
                 SWP_FRAMECHANGED);

    return false;
  }
}

}  // namespace freeaudio::clap_wrapper::standalone::windows::helper

#endif
