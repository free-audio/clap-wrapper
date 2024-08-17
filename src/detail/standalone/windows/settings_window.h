#pragma once

#include <Windows.h>
#include <wil/resource.h>

namespace freeaudio::clap_wrapper::standalone::windows
{
struct SettingsWindow
{
  SettingsWindow();

  static LRESULT CALLBACK wndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  int onCreate();
  int onClose();

  wil::unique_hwnd m_hWnd;
};
}  // namespace freeaudio::clap_wrapper::standalone::windows
