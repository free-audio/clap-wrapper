#pragma once

#include <Windows.h>
#include <ShlObj.h>
#include <wil/resource.h>

#include "detail/standalone/standalone_host.h"
// #include "settings_window.h"

namespace freeaudio::clap_wrapper::standalone::windows
{
struct HostWindow
{
  HostWindow(std::shared_ptr<Clap::Plugin> plugin);

  void setupMenu();
  void setupStandaloneHost();
  void setupPlugin();

  bool setWindowSize(uint32_t width, uint32_t height);

  static LRESULT CALLBACK wndProc(::HWND hWnd, ::UINT uMsg, ::WPARAM wParam, ::LPARAM lParam);
  int onDpiChanged();
  int onWindowPosChanged(::LPARAM lParam);
  int onSysCommand(::HWND hWnd, ::UINT uMsg, ::WPARAM wParam, ::LPARAM lParam);
  int onDestroy();

  std::shared_ptr<Clap::Plugin> m_clapPlugin;
  const clap_plugin_t* m_plugin;
  const clap_plugin_gui_t* m_pluginGui;
  const clap_plugin_state_t* m_pluginState;
  freeaudio::clap_wrapper::standalone::StandaloneHost* m_standaloneHost;
  std::vector<COMDLG_FILTERSPEC> m_fileTypes{{L"clapwrapper", L"*.clapwrapper"}};

  // SettingsWindow m_settingsWindow;
  wil::unique_hwnd m_hWnd;
};
}  // namespace freeaudio::clap_wrapper::standalone::windows
