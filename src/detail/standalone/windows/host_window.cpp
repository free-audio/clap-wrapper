#include <wil/com.h>
#include "detail/standalone/entry.h"
#include "host_window.h"
#include "helpers.h"

#define IDM_SETTINGS 1001
#define IDM_SAVE_STATE 1002
#define IDM_LOAD_STATE 1003
#define IDM_RESET_STATE 1004

namespace freeaudio::clap_wrapper::standalone::windows
{
HostWindow::HostWindow(std::shared_ptr<Clap::Plugin> clapPlugin)
  : m_clapPlugin{clapPlugin}
  , m_plugin{m_clapPlugin->_plugin}
  , m_pluginGui{m_clapPlugin->_ext._gui}
  , m_pluginState{m_clapPlugin->_ext._state}
{
  freeaudio::clap_wrapper::standalone::windows::helpers::createWindow(OUTPUT_NAME, this);

  setupMenu();

  setupStandaloneHost();

  setupPlugin();

  helpers::activateWindow(m_hWnd.get());
}

void HostWindow::setupMenu()
{
  auto hMenu{::GetSystemMenu(m_hWnd.get(), FALSE)};

  ::MENUITEMINFOW seperator{sizeof(::MENUITEMINFOW)};
  seperator.fMask = MIIM_FTYPE;
  seperator.fType = MFT_SEPARATOR;

  ::MENUITEMINFOW settings{sizeof(::MENUITEMINFOW)};
  settings.fMask = MIIM_STATE | MIIM_STRING | MIIM_ID;
  settings.wID = IDM_SETTINGS;
  settings.dwTypeData = const_cast<LPWSTR>(L"Audio/MIDI Settings");
  settings.fState = MFS_DISABLED;

  ::MENUITEMINFOW saveState{sizeof(::MENUITEMINFOW)};
  saveState.fMask = MIIM_STRING | MIIM_ID;
  saveState.wID = IDM_SAVE_STATE;
  saveState.dwTypeData = const_cast<LPWSTR>(L"Save plugin state...");

  ::MENUITEMINFOW loadState{sizeof(::MENUITEMINFOW)};
  loadState.fMask = MIIM_STRING | MIIM_ID;
  loadState.wID = IDM_LOAD_STATE;
  loadState.dwTypeData = const_cast<LPWSTR>(L"Load plugin state...");

  ::MENUITEMINFOW resetState{sizeof(::MENUITEMINFOW)};
  resetState.fMask = MIIM_STRING | MIIM_ID;
  resetState.wID = IDM_RESET_STATE;
  resetState.dwTypeData = const_cast<LPWSTR>(L"Reset to default plugin state");

  if (hMenu != INVALID_HANDLE_VALUE)
  {
    ::InsertMenuItemW(hMenu, 1, TRUE, &seperator);
    ::InsertMenuItemW(hMenu, 2, TRUE, &settings);
    ::InsertMenuItemW(hMenu, 3, TRUE, &seperator);
    ::InsertMenuItemW(hMenu, 4, TRUE, &saveState);
    ::InsertMenuItemW(hMenu, 5, TRUE, &loadState);
    ::InsertMenuItemW(hMenu, 6, TRUE, &seperator);
    ::InsertMenuItemW(hMenu, 7, TRUE, &resetState);
    ::InsertMenuItemW(hMenu, 8, TRUE, &seperator);
  }
}

void HostWindow::setupStandaloneHost()
{
  freeaudio::clap_wrapper::standalone::getStandaloneHost()->onRequestResize =
      [this](uint32_t width, uint32_t height) { return setWindowSize(width, height); };
}

void HostWindow::setupPlugin()
{
  m_pluginGui->create(m_plugin, CLAP_WINDOW_API_WIN32, false);

  m_pluginGui->set_scale(m_plugin, helpers::getScale(m_hWnd.get()));

  uint32_t width{0};
  uint32_t height{0};

  if (m_pluginGui->can_resize(m_plugin))
  {
    // We can restore size here
    // setWindowSize(previousWidth, previousHeight);
  }
  else
  {
    // We can't resize, so disable WS_THICKFRAME and WS_MAXIMIZEBOX
    ::SetWindowLongPtrW(m_hWnd.get(), GWL_STYLE,
                        ::GetWindowLongPtrW(m_hWnd.get(), GWL_STYLE) & ~WS_OVERLAPPEDWINDOW |
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
  }

  m_pluginGui->get_size(m_plugin, &width, &height);

  setWindowSize(width, height);

  clap_window clapWindow{CLAP_WINDOW_API_WIN32, static_cast<void*>(m_hWnd.get())};

  m_pluginGui->set_parent(m_plugin, &clapWindow);
}

bool HostWindow::setWindowSize(uint32_t width, uint32_t height)
{
  ::RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

  ::AdjustWindowRectExForDpi(&rect, static_cast<::DWORD>(::GetWindowLongPtrW(m_hWnd.get(), GWL_STYLE)),
                             ::GetMenu(m_hWnd.get()) != nullptr,
                             static_cast<::DWORD>(::GetWindowLongPtrW(m_hWnd.get(), GWL_EXSTYLE)),
                             helpers::getCurrentDpi(m_hWnd.get()));

  ::SetWindowPos(m_hWnd.get(), nullptr, 0, 0, (rect.right - rect.left), (rect.bottom - rect.top),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);

  return true;
}

LRESULT CALLBACK HostWindow::wndProc(::HWND hWnd, ::UINT uMsg, ::WPARAM wParam, ::LPARAM lParam)
{
  auto self{helpers::instanceFromWndProc<HostWindow>(hWnd, uMsg, lParam)};

  if (self)
  {
    switch (uMsg)
    {
      case WM_DPICHANGED:
        return self->onDpiChanged();
      case WM_WINDOWPOSCHANGED:
        return self->onWindowPosChanged(lParam);
      case WM_SYSCOMMAND:
        return self->onSysCommand(hWnd, uMsg, wParam, lParam);
      case WM_DESTROY:
        return self->onDestroy();
    }
  }

  return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

int HostWindow::onDpiChanged()
{
  m_pluginGui->set_scale(m_plugin, helpers::getScale(m_hWnd.get()));

  return 0;
}

int HostWindow::onWindowPosChanged(::LPARAM lParam)
{
  auto windowPos{reinterpret_cast<::LPWINDOWPOS>(lParam)};

  if (windowPos->flags & SWP_SHOWWINDOW)
  {
    m_pluginGui->show(m_plugin);
  }

  if (windowPos->flags & SWP_HIDEWINDOW)
  {
    m_pluginGui->hide(m_plugin);
  }

  if (m_pluginGui->can_resize(m_plugin))
  {
    auto size{helpers::getClientSize(m_hWnd.get())};

    m_pluginGui->adjust_size(m_plugin, &size.width, &size.height);
    m_pluginGui->set_size(m_plugin, size.width, size.height);
  }

  return 0;
}

int HostWindow::onSysCommand(::HWND hWnd, ::UINT uMsg, ::WPARAM wParam, ::LPARAM lParam)
{
  switch (wParam)
  {
    case IDM_SETTINGS:
    {
      // Disabled until settings window is finished
      // helpers::checkWindowVisibility(m_settingsWindow.m_hWnd.get())
      //     ? helpers::hideWindow(m_settingsWindow.m_hWnd.get())
      //     : helpers::showWindow(m_settingsWindow.m_hWnd.get());

      return 0;
    }

    case IDM_SAVE_STATE:
    {
      auto coUninitialize{wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};

      auto fileSaveDialog{wil::CoCreateInstance<IFileSaveDialog>(CLSID_FileSaveDialog)};

      fileSaveDialog->SetDefaultExtension(m_fileTypes.at(0).pszName);
      fileSaveDialog->SetFileTypes(static_cast<UINT>(m_fileTypes.size()), m_fileTypes.data());

      fileSaveDialog->Show(m_hWnd.get());

      wil::com_ptr<IShellItem> shellItem;
      auto hr{fileSaveDialog->GetResult(&shellItem)};

      if (SUCCEEDED(hr))
      {
        wil::unique_cotaskmem_string result;
        shellItem->GetDisplayName(SIGDN_FILESYSPATH, &result);

        auto saveFile{std::filesystem::path(result.get())};

        helpers::log("{}", saveFile.string());

        try
        {
          freeaudio::clap_wrapper::standalone::getStandaloneHost()->saveStandaloneAndPluginSettings(
              saveFile.parent_path(), saveFile.filename());
        }
        catch (const fs::filesystem_error& e)
        {
          helpers::errorBox("Unable to save state: {}", e.what());
        }
      }

      return 0;
    }

    case IDM_LOAD_STATE:
    {
      auto coUninitialize{wil::CoInitializeEx(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)};

      auto fileOpenDialog{wil::CoCreateInstance<IFileOpenDialog>(CLSID_FileOpenDialog)};

      fileOpenDialog->SetDefaultExtension(m_fileTypes.at(0).pszName);
      fileOpenDialog->SetFileTypes(static_cast<UINT>(m_fileTypes.size()), m_fileTypes.data());

      fileOpenDialog->Show(m_hWnd.get());

      wil::com_ptr<IShellItem> shellItem;
      auto hr{fileOpenDialog->GetResult(&shellItem)};

      if (SUCCEEDED(hr))
      {
        wil::unique_cotaskmem_string result;
        shellItem->GetDisplayName(SIGDN_FILESYSPATH, &result);

        auto saveFile{std::filesystem::path(result.get())};

        helpers::log("{}", saveFile.string());

        try
        {
          if (fs::exists(saveFile))
          {
            freeaudio::clap_wrapper::standalone::getStandaloneHost()->tryLoadStandaloneAndPluginSettings(
                saveFile.parent_path(), saveFile.filename());
          }
        }
        catch (const fs::filesystem_error& e)
        {
          helpers::errorBox("Unable to load state: {}", e.what());
        }
      }

      return 0;
    }

    case IDM_RESET_STATE:
    {
      auto pt{freeaudio::clap_wrapper::standalone::getStandaloneSettingsPath()};

      if (pt.has_value())
      {
        auto loadPath{*pt / m_plugin->desc->id};

        try
        {
          if (fs::exists(loadPath / "defaults.clapwrapper"))
          {
            freeaudio::clap_wrapper::standalone::getStandaloneHost()->tryLoadStandaloneAndPluginSettings(
                loadPath, "defaults.clapwrapper");
          }
        }
        catch (const fs::filesystem_error& /* e */)
        {
          //
        }
      }

      return 0;
    }
  }

  ::DefWindowProcW(hWnd, uMsg, wParam, lParam);

  return 0;
}

int HostWindow::onDestroy()
{
  m_pluginGui->destroy(m_plugin);

  freeaudio::clap_wrapper::standalone::mainFinish();

  helpers::quit();

  return 0;
}
}  // namespace freeaudio::clap_wrapper::standalone::windows
