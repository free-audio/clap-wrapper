#include "windows_standalone.h"

namespace freeaudio::clap_wrapper::standalone::windows_standalone
{
std::pair<int, std::vector<std::string>> getArgs()
{
  int argc{0};
  wil::unique_hlocal_ptr<wchar_t*[]> buffer;
  buffer.reset(::CommandLineToArgvW(::GetCommandLineW(), &argc));

  std::vector<std::string> argv;

  for (int i = 0; i < argc; i++)
  {
    argv.emplace_back(toUTF8(buffer[i]));
  }

  return {argc, argv};
}

::HMODULE getInstance()
{
  ::HMODULE module;
  ::GetModuleHandleExW(
      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
      (LPCWSTR)&getInstance, &module);

  return module;
}

::HFONT getFontFromSystem(int name)
{
  return static_cast<::HFONT>(::GetStockObject(name));
}

::HFONT getScaledFontFromSystem(double scale)
{
  ::LOGFONTW fontAttributes{};
  ::GetObjectW(getFontFromSystem(), sizeof(fontAttributes), &fontAttributes);
  fontAttributes.lfHeight = static_cast<::LONG>(fontAttributes.lfHeight * scale);

  return ::CreateFontIndirectW(&fontAttributes);
}

::HBRUSH getBrushFromSystem(int name)
{
  return static_cast<::HBRUSH>(::GetStockObject(name));
}

::HCURSOR getCursorFromSystem(::LPCWSTR name)
{
  return static_cast<::HCURSOR>(
      ::LoadImageW(nullptr, name, IMAGE_CURSOR, 0, 0, LR_SHARED | LR_DEFAULTSIZE));
}

::HICON getIconFromSystem(::LPCWSTR name)
{
  return static_cast<::HICON>(::LoadImageW(nullptr, name, IMAGE_ICON, 0, 0, LR_SHARED | LR_DEFAULTSIZE));
}

::HICON getIconFromResource()
{
  return static_cast<::HICON>(
      ::LoadImageW(getInstance(), MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_SHARED | LR_DEFAULTSIZE));
}

::HMENU getSystemMenu(::HWND hwnd)
{
  return ::GetSystemMenu(hwnd, FALSE);
}

std::wstring toUTF16(std::string_view input)
{
  std::wstring output;

  if (input.length() > 0)
  {
    if (input.length() < std::numeric_limits<int>::max())
    {
      auto length{static_cast<int>(input.length())};

      length = ::MultiByteToWideChar(CP_UTF8, 0, input.data(), length, nullptr, 0);

      output.resize(length);

      if (::MultiByteToWideChar(CP_UTF8, 0, input.data(), length, output.data(), length) == 0)
      {
        log("toUTF16(): {}", getLastError());
      }
    }
    else
    {
      log("toUTF16(): String too long");
    }
  }

  return output;
}

std::string toUTF8(std::wstring_view input)
{
  std::string output;

  if (input.length() > 0)
  {
    if (input.length() < std::numeric_limits<int>::max())
    {
      auto length{static_cast<int>(input.length())};

      length = ::WideCharToMultiByte(CP_UTF8, 0, input.data(), length, nullptr, 0, nullptr, nullptr);

      output.resize(length);

      if (::WideCharToMultiByte(CP_UTF8, 0, input.data(), length, output.data(), length, nullptr,
                                nullptr) == 0)
      {
        log("toUTF8(): {}", getLastError());
      }
    }
    else
    {
      log("toUTF8(): String too long");
    }
  }

  return output;
}

std::string formatMessage(::HRESULT errorCode)
{
  wil::unique_hlocal_string buffer;

  ::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                   nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   wil::out_param_ptr<::LPWSTR>(buffer), 0, nullptr);

  return toUTF8(buffer.get());
}

std::string getLastError()
{
  return formatMessage(::GetLastError());
}

void log(const std::string& message)
{
  ::OutputDebugStringW(toUTF16(message).c_str());
  ::OutputDebugStringW(L"\n");
}

void log(const std::wstring& message)
{
  ::OutputDebugStringW(message.c_str());
  ::OutputDebugStringW(L"\n");
}

int run()
{
  ::MSG msg{};
  int r{};

  while ((r = ::GetMessageW(&msg, nullptr, 0, 0)) != 0)
  {
    if (r == -1)
    {
      return EXIT_FAILURE;
    }

    else
    {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  return static_cast<int>(msg.wParam);
}

void abort(int exitCode)
{
  ::ExitProcess(exitCode);
}

void quit(int exitCode)
{
  ::PostQuitMessage(exitCode);
}

bool MessageHandler::on(::UINT msg, MessageCallback callback)
{
  auto emplace{map.try_emplace(msg, callback)};

  return emplace.second;
}

bool MessageHandler::contains(::UINT msg)
{
  return (map.find(msg) != map.end());
}

::LRESULT MessageHandler::invoke(Message message)
{
  return map.find(message.msg)->second({message.hwnd, message.msg, message.wparam, message.lparam});
}

void MessageHandler::box(const std::string& message)
{
  ::MessageBoxW(nullptr, toUTF16(message).c_str(), nullptr, MB_OK | MB_ICONASTERISK);
}

void MessageHandler::box(const std::wstring& message)
{
  ::MessageBoxW(nullptr, message.c_str(), nullptr, MB_OK | MB_ICONASTERISK);
}

void MessageHandler::error(const std::string& message)
{
  ::MessageBoxW(nullptr, toUTF16(message).c_str(), nullptr, MB_OK | MB_ICONHAND);
}

void MessageHandler::error(const std::wstring& message)
{
  ::MessageBoxW(nullptr, message.c_str(), nullptr, MB_OK | MB_ICONHAND);
}

void Window::create(const std::string& title)
{
  std::wstring className{L"Window"};

  auto instance{getInstance()};
  auto resourceIcon{getIconFromResource()};
  auto systemIcon{getIconFromSystem()};
  auto systemCursor{getCursorFromSystem()};
  auto systemBrush{getBrushFromSystem()};

  WNDCLASSEXW windowClass{sizeof(::WNDCLASSEXW)};
  windowClass.style = 0;
  windowClass.lpfnWndProc = procedure;
  windowClass.cbClsExtra = 0;
  windowClass.cbWndExtra = sizeof(void*);
  windowClass.hInstance = instance;
  windowClass.hIcon = resourceIcon ? resourceIcon : systemIcon;
  windowClass.hCursor = systemCursor;
  windowClass.hbrBackground = systemBrush;
  windowClass.lpszMenuName = nullptr;
  windowClass.lpszClassName = className.c_str();
  windowClass.hIconSm = resourceIcon ? resourceIcon : systemIcon;

  if (::GetClassInfoExW(instance, className.c_str(), &windowClass) == 0)
  {
    ::RegisterClassExW(&windowClass);
  }

  ::CreateWindowExW(0, className.c_str(), toUTF16(title).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                    instance, this);
}

::LRESULT CALLBACK Window::procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam)
{
  if (msg == WM_NCCREATE)
  {
    auto create{reinterpret_cast<::CREATESTRUCTW*>(lparam)};

    if (auto self{static_cast<Window*>(create->lpCreateParams)}; self)
    {
      ::SetWindowLongPtrW(hwnd, 0, reinterpret_cast<::LONG_PTR>(self));
      self->hwnd.reset(hwnd);
      self->dpi = static_cast<uint32_t>(::GetDpiForWindow(hwnd));
      self->scale = (static_cast<double>(self->dpi) / static_cast<double>(USER_DEFAULT_SCREEN_DPI));
    }
  }

  if (auto self{reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, 0))}; self)
  {
    if (msg == WM_NCDESTROY)
    {
      ::SetWindowLongPtrW(hwnd, 0, reinterpret_cast<::LONG_PTR>(nullptr));
    }

    if (msg == WM_WINDOWPOSCHANGED)
    {
      auto windowPos{reinterpret_cast<::LPWINDOWPOS>(lparam)};

      self->window.x = windowPos->x;
      self->window.y = windowPos->y;
      self->window.width = windowPos->cx;
      self->window.height = windowPos->cy;

      ::GetWindowPlacement(hwnd, &self->placement);

      ::RECT rect{};
      ::GetClientRect(hwnd, &rect);

      self->client.x = rect.left;
      self->client.y = rect.top;
      self->client.width = rect.right - rect.left;
      self->client.height = rect.bottom - rect.top;

      ::MONITORINFO mi{sizeof(::MONITORINFO)};
      ::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);

      self->monitor.x = mi.rcWork.left;
      self->monitor.y = mi.rcWork.top;
      self->monitor.width = mi.rcWork.right - mi.rcWork.left;
      self->monitor.height = mi.rcWork.bottom - mi.rcWork.top;
    }

    if (msg == WM_DPICHANGED)
    {
      auto rect{reinterpret_cast<::LPRECT>(lparam)};

      self->suggested.x = rect->left;
      self->suggested.y = rect->top;
      self->suggested.width = rect->left - rect->right;
      self->suggested.height = rect->bottom - rect->top;

      self->dpi = static_cast<uint32_t>(::GetDpiForWindow(hwnd));
      self->scale = (static_cast<double>(self->dpi) / static_cast<double>(USER_DEFAULT_SCREEN_DPI));
    }

    if (self->message.contains(msg))
    {
      return self->message.invoke({hwnd, msg, wparam, lparam});
    }

    if (msg == WM_CLOSE)
    {
      self->hwnd.reset();

      return 0;
    }
  }

  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void Window::activate()
{
  ::ShowWindow(hwnd.get(), SW_NORMAL);
}

void Window::show()
{
  ::ShowWindow(hwnd.get(), SW_SHOW);
}

void Window::hide()
{
  ::ShowWindow(hwnd.get(), SW_HIDE);
}

bool Window::isVisible()
{
  return ::IsWindowVisible(hwnd.get());
}

void Window::adjustSize(uint32_t width, uint32_t height)
{
  ::RECT rect{};
  rect.left = 0;
  rect.top = 0;
  rect.right = width;
  rect.bottom = height;

  ::AdjustWindowRectExForDpi(&rect, static_cast<::DWORD>(::GetWindowLongPtrW(hwnd.get(), GWL_STYLE)),
                             ::GetMenu(hwnd.get()) != nullptr,
                             static_cast<::DWORD>(::GetWindowLongPtrW(hwnd.get(), GWL_EXSTYLE)), dpi);

  ::SetWindowPos(hwnd.get(), nullptr, 0, 0, (rect.right - rect.left), (rect.bottom - rect.top),
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

void Window::setPosition(Position position)
{
  ::SetWindowPos(hwnd.get(), nullptr, position.x, position.y, position.width, position.height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Window::setStyle(::LONG_PTR style)
{
  ::SetWindowLongPtrW(hwnd.get(), GWL_STYLE, style);
}

void Window::toggleCentered(bool centered)
{
  restore = window;

  if (centered)
  {
    if (monitor.width > window.width && monitor.height > window.height)
    {
      auto x{static_cast<int>((monitor.width - window.width) / 2)};
      auto y{static_cast<int>((monitor.height - window.height) / 2)};

      ::SetWindowPos(hwnd.get(), nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    }
  }
  else
  {
    setPosition(restore);
  }
}

void Window::toggleTopmost(bool topmost)
{
  ::SetWindowPos(hwnd.get(), topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
}

void Window::repaint()
{
  ::RECT r{};
  ::GetClientRect(hwnd.get(), &r);
  ::InvalidateRect(hwnd.get(), &r, true);
}

bool Window::startTimer(::UINT_PTR timerId, ::UINT intervalMs)
{
  return ::SetTimer(hwnd.get(), timerId, intervalMs, nullptr) != 0 ? true : false;
}

bool Window::stopTimer(::UINT_PTR timerId)
{
  return ::KillTimer(hwnd.get(), timerId);
}

void Control::setPosition(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
  position.x = x;
  position.y = y;
  position.width = width;
  position.height = height;

  ::SetWindowPos(hwnd.get(), nullptr, position.x, position.y, position.width, position.height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Control::refreshFont(double scale)
{
  font.reset(getScaledFontFromSystem(scale));
  message.send(hwnd.get(), WM_SETFONT, font.get(), TRUE);
}

void ComboBox::create(const std::string& name, uintptr_t id, ::HWND parentHwnd)
{
  hwnd.reset(::CreateWindowExW(0, WC_COMBOBOXW, toUTF16(name).c_str(),
                               WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 0, 0, 0, 0, parentHwnd,
                               (::HMENU)id, getInstance(), nullptr));

  ::SetWindowSubclass(hwnd.get(), &procedure, id, reinterpret_cast<::DWORD_PTR>(this));

  message.send(hwnd.get(), WM_SETFONT, getFontFromSystem(), TRUE);
}

void ComboBox::reset()
{
  message.send(hwnd.get(), CB_RESETCONTENT, 0, 0);
}

void ComboBox::add(const std::string& string)
{
  message.send(hwnd.get(), CB_ADDSTRING, 0, toUTF16(string).c_str());
}

bool ComboBox::set(int index)
{
  return message.send(hwnd.get(), CB_SETCURSEL, index, 0) != CB_ERR ? true : false;
}

bool ComboBox::set(const std::string& searchString)
{
  return message.send(hwnd.get(), CB_SELECTSTRING, -1, toUTF16(searchString).c_str()) != CB_ERR ? true
                                                                                                : false;
}

::LRESULT ComboBox::get()
{
  return message.send(hwnd.get(), CB_GETCURSEL, 0, 0);
}

::LRESULT ComboBox::getItemHeight()
{
  return message.send(hwnd.get(), CB_GETITEMHEIGHT, 0, 0);
}

::LRESULT CALLBACK ComboBox::procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam,
                                       ::UINT_PTR id, ::DWORD_PTR data)
{
  if (auto self{reinterpret_cast<ComboBox*>(data)}; self)
  {
    if (msg == WM_WINDOWPOSCHANGED)
    {
      auto windowPos{reinterpret_cast<::LPWINDOWPOS>(lparam)};

      self->position.x = windowPos->x;
      self->position.y = windowPos->y;
      self->position.width = windowPos->cx;
      self->position.height = windowPos->cy;
    }
  }

  return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

void ListBox::create(const std::string& name, uintptr_t id, ::HWND parentHwnd)
{
  hwnd.reset(::CreateWindowExW(0, WC_LISTBOXW, toUTF16(name).c_str(),
                               WS_CHILD | WS_VISIBLE | LBS_MULTIPLESEL | LBS_NOTIFY, 0, 0, 0, 0,
                               parentHwnd, (::HMENU)id, getInstance(), nullptr));

  ::SetWindowSubclass(hwnd.get(), &procedure, id, reinterpret_cast<::DWORD_PTR>(this));

  message.send(hwnd.get(), WM_SETFONT, getFontFromSystem(), TRUE);
}

void ListBox::reset()
{
  message.send(hwnd.get(), LB_RESETCONTENT, 0, 0);
}

void ListBox::add(const std::string& string)
{
  message.send(hwnd.get(), LB_ADDSTRING, 0, toUTF16(string).c_str());
}

bool ListBox::set(int index)
{
  return message.send(hwnd.get(), LB_SETCURSEL, index, 0) != CB_ERR ? true : false;
}

bool ListBox::set(const std::string& searchString)
{
  return message.send(hwnd.get(), LB_SELECTSTRING, -1, toUTF16(searchString).c_str()) != CB_ERR ? true
                                                                                                : false;
}

::LRESULT ListBox::get()
{
  return message.send(hwnd.get(), LB_GETCURSEL, 0, 0);
}

::LRESULT ListBox::getItems(std::vector<int>& buffer)
{
  buffer.resize(getItemsCount());
  return message.send(hwnd.get(), LB_GETSELITEMS, getItemsCount(), buffer.data());
}

::LRESULT ListBox::getItemsCount()
{
  return message.send(hwnd.get(), LB_GETSELCOUNT, 0, 0);
}

::LRESULT ListBox::getItemHeight()
{
  return message.send(hwnd.get(), LB_GETITEMHEIGHT, 0, 0);
}

::LRESULT CALLBACK ListBox::procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam,
                                      ::UINT_PTR id, ::DWORD_PTR data)
{
  if (auto self{reinterpret_cast<ComboBox*>(data)}; self)
  {
    if (msg == WM_WINDOWPOSCHANGED)
    {
      auto windowPos{reinterpret_cast<::LPWINDOWPOS>(lparam)};

      self->position.x = windowPos->x;
      self->position.y = windowPos->y;
      self->position.width = windowPos->cx;
      self->position.height = windowPos->cy;
    }
  }

  return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

void SystemMenu::add(std::wstring& name, ::UINT id)
{
  item.emplace_back(::MENUITEMINFOW{sizeof(::MENUITEMINFOW), MIIM_STRING | MIIM_ID, 0, 0, id, nullptr,
                                    nullptr, nullptr, 0, name.data(), 0, nullptr});
}

void SystemMenu::addToggle(std::wstring& name, ::UINT id, bool checked)
{
  item.emplace_back(::MENUITEMINFOW{sizeof(::MENUITEMINFOW),
                                    MIIM_STRING | MIIM_ID | MIIM_CHECKMARKS | MIIM_STATE, 0,
                                    static_cast<::UINT>(checked ? MFS_CHECKED : MFS_UNCHECKED), id,
                                    nullptr, nullptr, nullptr, 0, name.data(), 0, nullptr});
}

void SystemMenu::addSeparator()
{
  item.emplace_back(::MENUITEMINFOW{sizeof(::MENUITEMINFOW), MIIM_FTYPE, MFT_SEPARATOR, 0, 0, nullptr,
                                    nullptr, nullptr, 0, nullptr, 0, nullptr});
}

void SystemMenu::populate(::HWND hwnd)
{
  if (auto systemMenu{getSystemMenu(hwnd)}; systemMenu != INVALID_HANDLE_VALUE)
  {
    for (int i{0}; i < item.size(); i++)
    {
      ::InsertMenuItemW(systemMenu, i, TRUE, &item[i]);
    }
  }
}

Plugin::Plugin(std::shared_ptr<Clap::Plugin> clapPlugin)
{
  plugin.clap = clapPlugin;
  plugin.plugin = plugin.clap->_plugin;
  plugin.gui = plugin.clap->_ext._gui;
  plugin.state = plugin.clap->_ext._state;

  message.on(WM_CREATE,
             [this](Message msg)
             {
               menu.add(menu.audioMidiSettings, Menu::Identifier::AudioMidiSettings);
               menu.addToggle(menu.muteInput, Menu::Identifier::MuteInput);
               menu.addSeparator();
               menu.add(menu.saveState, Menu::Identifier::SaveState);
               menu.add(menu.loadState, Menu::Identifier::LoadState);
               menu.add(menu.resetState, Menu::Identifier::ResetState);
               menu.addSeparator();

               menu.populate(hwnd.get());

               return 0;
             });

  settings.message.on(
      WM_CREATE,
      [this](Message msg)
      {
        settings.setStyle(WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);

        settings.api.create("API", Settings::Identifier::AudioApi, settings.hwnd.get());
        settings.output.create("Output", Settings::Identifier::AudioOutput, settings.hwnd.get());
        settings.input.create("Input", Settings::Identifier::AudioInput, settings.hwnd.get());
        settings.sampleRate.create("Sample Rate", Settings::Identifier::AudioSamplerate,
                                   settings.hwnd.get());
        settings.bufferSize.create("Buffer Size", Settings::Identifier::AudioBuffersize,
                                   settings.hwnd.get());
        settings.midiIn.create("MIDI Inputs", Settings::Identifier::MidiInputs, settings.hwnd.get());

        return 0;
      });

  message.on(WM_WINDOWPOSCHANGED,
             [this](Message msg)
             {
               auto windowPos{reinterpret_cast<::LPWINDOWPOS>(msg.lparam)};

               if (plugin.gui)
               {
                 if (windowPos->flags & SWP_SHOWWINDOW)
                 {
                   plugin.gui->show(plugin.plugin);
                 }

                 if (windowPos->flags & SWP_HIDEWINDOW)
                 {
                   plugin.gui->hide(plugin.plugin);
                 }

                 if (plugin.gui->can_resize(plugin.plugin))
                 {
                   plugin.gui->adjust_size(plugin.plugin, &client.width, &client.height);
                   plugin.gui->set_size(plugin.plugin, client.width, client.height);
                 }
               }

               position.x = windowPos->x;
               position.y = windowPos->y;
               position.width = windowPos->cx;
               position.height = windowPos->cy;

               saveSettings();

               return 0;
             });

  settings.message.on(WM_WINDOWPOSCHANGED,
                      [this](Message msg)
                      {
                        auto windowPos{reinterpret_cast<::LPWINDOWPOS>(msg.lparam)};

                        if (windowPos->flags & SWP_SHOWWINDOW)
                        {
                          settings.toggleCentered(true);
                        }

                        return 0;
                      });

  message.on(WM_DPICHANGED,
             [this](Message msg)
             {
               if (plugin.gui)
               {
                 plugin.gui->set_scale(plugin.plugin, scale);
               }

               return 0;
             });

  settings.message.on(WM_DPICHANGED,
                      [this](Message msg)
                      {
                        refreshLayout();

                        return 0;
                      });

  message.on(WM_SYSCOMMAND,
             [this](Message msg)
             {
               switch (msg.wparam)
               {
                 case Menu::Identifier::AudioMidiSettings:
                 {
                   settings.isVisible() ? settings.hide() : settings.show();

                   return 0;
                 }

                 case Menu::Identifier::MuteInput:
                 {
                   if (menu.item[1].fState == MFS_UNCHECKED)
                   {
                     sah->audioInputUsed = false;
                     menu.item[1].fState = MFS_CHECKED;
                   }
                   else
                   {
                     sah->audioInputUsed = true;
                     menu.item[1].fState = MFS_UNCHECKED;
                   }

                   SetMenuItemInfoW(getSystemMenu(hwnd.get()), 1, FALSE, &menu.item[1]);

                   saveSettings();
                   startAudio();

                   return 0;
                 }

                 case Menu::Identifier::SaveState:
                 {
                   auto fileSaveDialog{wil::CoCreateInstance<::IFileSaveDialog>(CLSID_FileSaveDialog)};

                   fileSaveDialog->SetDefaultExtension(fileTypes.at(0).pszName);
                   fileSaveDialog->SetFileTypes(static_cast<::UINT>(fileTypes.size()), fileTypes.data());
                   fileSaveDialog->Show(hwnd.get());

                   wil::com_ptr<::IShellItem> shellItem;

                   if (auto hr{fileSaveDialog->GetResult(&shellItem)}; SUCCEEDED(hr))
                   {
                     wil::unique_cotaskmem_string result;
                     shellItem->GetDisplayName(SIGDN_FILESYSPATH, &result);

                     auto saveFile{std::filesystem::path(result.get())};

                     try
                     {
                       sah->saveStandaloneAndPluginSettings(saveFile.parent_path(), saveFile.filename());
                     }
                     catch (const fs::filesystem_error& e)
                     {
                       message.error("Unable to save state: {}", e.what());
                     }
                   }

                   return 0;
                 }

                 case Menu::Identifier::LoadState:
                 {
                   auto fileOpenDialog{wil::CoCreateInstance<::IFileOpenDialog>(CLSID_FileOpenDialog)};

                   fileOpenDialog->SetDefaultExtension(fileTypes.at(0).pszName);
                   fileOpenDialog->SetFileTypes(static_cast<::UINT>(fileTypes.size()), fileTypes.data());
                   fileOpenDialog->Show(hwnd.get());

                   wil::com_ptr<::IShellItem> shellItem;

                   if (auto hr{fileOpenDialog->GetResult(&shellItem)}; SUCCEEDED(hr))
                   {
                     wil::unique_cotaskmem_string result;
                     shellItem->GetDisplayName(SIGDN_FILESYSPATH, &result);

                     auto saveFile{std::filesystem::path(result.get())};

                     try
                     {
                       if (fs::exists(saveFile))
                       {
                         sah->tryLoadStandaloneAndPluginSettings(saveFile.parent_path(),
                                                                 saveFile.filename());
                       }
                     }
                     catch (const fs::filesystem_error& e)
                     {
                       message.error("Unable to load state: {}", e.what());
                     }
                   }

                   return 0;
                 }

                 case Menu::Identifier::ResetState:
                 {
                   auto pt{freeaudio::clap_wrapper::standalone::getStandaloneSettingsPath()};

                   if (pt.has_value())
                   {
                     auto loadPath{*pt / plugin.plugin->desc->id};

                     try
                     {
                       if (fs::exists(loadPath / "defaults.clapwrapper"))
                       {
                         sah->tryLoadStandaloneAndPluginSettings(loadPath, "defaults.clapwrapper");
                       }
                     }
                     catch (const fs::filesystem_error& e)
                     {
                       message.error("Unable to reset state: {}", e.what());
                     }
                   }

                   return 0;
                 }
               }

               ::DefWindowProcW(msg.hwnd, msg.msg, msg.wparam, msg.lparam);

               return 0;
             });

  settings.message.on(
      WM_COMMAND,
      [this](Message msg)
      {
        if (HIWORD(msg.wparam) == CBN_SELCHANGE)
        {
          if (LOWORD(msg.wparam) == Settings::Identifier::AudioApi)
          {
            initializeAudio(sah->getCompiledApi()[settings.api.get()]);

            refreshOutputs();
            refreshInputs();

            settings.output.set(sah->rtaDac->getDeviceInfo(sah->audioOutputDeviceID).name);
            settings.input.set(sah->rtaDac->getDeviceInfo(sah->audioInputDeviceID).name);

            refreshSampleRates();
            refreshBufferSizes();

            settings.sampleRate.set(std::to_string(sah->currentSampleRate));
            settings.bufferSize.set(std::to_string(sah->currentBufferSize));

            saveSettings();
            startAudio();
          }

          if (LOWORD(msg.wparam) == Settings::Identifier::AudioOutput)
          {
            sah->audioOutputDeviceID = sah->getOutputAudioDevices()[settings.output.get()].ID;

            sah->totalOutputChannels =
                sah->getOutputAudioDevices()[settings.output.get()].outputChannels;

            refreshSampleRates();
            refreshBufferSizes();

            saveSettings();
            startAudio();
          }

          if (LOWORD(msg.wparam) == Settings::Identifier::AudioInput)
          {
            sah->audioInputDeviceID = sah->getInputAudioDevices()[settings.input.get()].ID;

            sah->totalInputChannels = sah->getInputAudioDevices()[settings.input.get()].inputChannels;

            refreshSampleRates();
            refreshBufferSizes();

            saveSettings();
            startAudio();
          }

          if (LOWORD(msg.wparam) == Settings::Identifier::AudioSamplerate)
          {
            auto sampleRates{sah->getInputAudioDevices()[settings.input.get()].sampleRates};

            auto newRate{sampleRates[settings.sampleRate.get()]};

            sah->currentSampleRate = newRate;

            saveSettings();
            startAudio();
          }

          if (LOWORD(msg.wparam) == Settings::Identifier::AudioBuffersize)
          {
            auto bufferSizes{sah->getBufferSizes()};

            auto bufferSize{bufferSizes[settings.bufferSize.get()]};

            sah->currentBufferSize = bufferSize;

            saveSettings();
            startAudio();
          }
        }

        if (HIWORD(msg.wparam) == LBN_SELCHANGE)
        {
          if (LOWORD(msg.wparam) == Settings::Identifier::MidiInputs)
          {
            std::vector<int> ports;
            settings.midiIn.getItems(ports);

            for (auto& midiIn : sah->midiIns)
            {
              midiIn.reset();
            }
            sah->midiIns.clear();
            sah->currentMidiPorts.clear();

            for (auto port : ports)
            {
              if (ports.size() != 0)
              {
                try
                {
                  auto midiIn{std::make_unique<RtMidiIn>()};
                  midiIn->openPort(port);
                  midiIn->setCallback(sah->midiCallback, sah);
                  sah->midiIns.push_back(std::move(midiIn));
                  sah->currentMidiPorts.push_back(port);
                }
                catch (RtMidiError& error)
                {
                  log("{}", error.getMessage());
                };
              }
              else
              {
                sah->stopMIDIThread();
              }
            }

            saveSettings();
          }
        }

        return 0;
      });

  message.on(WM_TIMER,
             [this](Message msg)
             {
               if (msg.wparam == timerId)
               {
                 if (sah->callbackRequested.exchange(false))
                 {
                   plugin.plugin->on_main_thread(plugin.plugin);
                 }
               }

               return 0;
             });

  message.on(WM_DESTROY,
             [this](Message msg)
             {
               saveSettings();

               sah->onRequestResize = nullptr;
               sah->displayAudioError = nullptr;

               if (plugin.gui)
               {
                 plugin.gui->destroy(plugin.plugin);
               }

               if (isTimerRunning)
               {
                 if (stopTimer(timerId))
                 {
                   log(getLastError());
                 }
               }

               freeaudio::clap_wrapper::standalone::mainFinish();

               quit();

               return 0;
             });

  settings.message.on(WM_CLOSE,
                      [this](Message msg)
                      {
                        settings.hide();

                        return 0;
                      });

  settings.message.on(
      WM_PAINT,
      [this](Message msg)
      {
        ::PAINTSTRUCT ps;
        ::HDC hdc{::BeginPaint(msg.hwnd, &ps)};

        ::SetTextColor(hdc, RGB(255, 255, 255));
        ::SetBkMode(hdc, TRANSPARENT);

        auto font{getScaledFontFromSystem(settings.scale)};
        ::SelectObject(hdc, font);

        ::TextOutW(hdc, 10, 10, L"API: ", 5);
        ::TextOutW(hdc, 10, settings.api.position.y + settings.api.position.height + 10, L"Output: ", 8);
        ::TextOutW(hdc, 10, settings.output.position.y + settings.output.position.height + 10,
                   L"Input: ", 7);
        ::TextOutW(hdc, 10, settings.input.position.y + settings.input.position.height + 10,
                   L"Sample Rate: ", 13);
        ::TextOutW(hdc, 10, settings.sampleRate.position.y + settings.sampleRate.position.height + 10,
                   L"Buffer Size: ", 13);

        if (sah->numMidiPorts != 0)
        {
          ::TextOutW(hdc, 10, settings.bufferSize.position.y + settings.bufferSize.position.height + 10,
                     L"MIDI Inputs: ", 13);
        }

        ::EndPaint(msg.hwnd, &ps);

        return 0;
      });

  create(OUTPUT_NAME);

  settings.create("Audio/MIDI Settings");

  isTimerRunning = startTimer(timerId, 8);

  if (!isTimerRunning)
  {
    log(getLastError());
  }

  if (loadSettings())
  {
    initializeAudio();
    menu.item[1].fState = sah->audioInputUsed ? MFS_UNCHECKED : MFS_CHECKED;
    SetMenuItemInfoW(getSystemMenu(hwnd.get()), 1, FALSE, &menu.item[1]);
    sah->setAudioApi(sah->audioApi);
  }
  else
  {
    initializeAudio();
    saveSettings();
  }

  if (plugin.gui)
  {
    if (plugin.gui->is_api_supported(plugin.plugin, CLAP_WINDOW_API_WIN32, false))
    {
      plugin.gui->create(plugin.plugin, CLAP_WINDOW_API_WIN32, false);
      plugin.gui->set_scale(plugin.plugin, scale);

      if (plugin.gui->can_resize(plugin.plugin))
      {
        // We can restore size here
        // setWindowPosition(m_window.hwnd.get(), previousWidth, previousHeight);
        // log("x: {} y: {}", sah->position.x, sah->position.y);
        if (position.width != 0 || position.height != 0)
        {
          setPosition(position);
        }
      }
      else
      {
        // We can't resize, so disable WS_THICKFRAME and WS_MAXIMIZEBOX
        setStyle(WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
      }

      Position pluginSize;
      plugin.gui->get_size(plugin.plugin, &pluginSize.width, &pluginSize.height);
      log("{}, {}", pluginSize.width, pluginSize.height);

      adjustSize(pluginSize.width, pluginSize.height);

      clap_window clapWindow{CLAP_WINDOW_API_WIN32, static_cast<void*>(hwnd.get())};
      plugin.gui->set_parent(plugin.plugin, &clapWindow);
    }
    else
    {
      log("CLAP_WINDOW_API_WIN32 is not supported");
    }
  }
  else
  {
    setStyle(WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
    adjustSize(static_cast<uint32_t>(500 * scale), 0);
    toggleCentered(true);
  }

  sah->onRequestResize = [this](uint32_t width, uint32_t height)
  {
    if (placement.showCmd != SW_MAXIMIZE)
    {
      adjustSize(width, height);
    }

    return true;
  };

  startMIDI();
  refreshMIDIInputs();

  sah->displayAudioError = [this](auto& errorText)
  { message.error("Unable to configure audio: {}", errorText); };

  refreshApis();
  refreshOutputs();
  refreshInputs();
  refreshSampleRates();
  refreshBufferSizes();

  settings.api.set(sah->audioApiDisplayName);
  settings.output.set(sah->rtaDac->getDeviceInfo(sah->audioOutputDeviceID).name);
  settings.input.set(sah->rtaDac->getDeviceInfo(sah->audioInputDeviceID).name);
  settings.sampleRate.set(std::to_string(sah->currentSampleRate));
  settings.bufferSize.set(std::to_string(sah->currentBufferSize));

  refreshLayout();

  startAudio();

  activate();
}

std::optional<clap_gui_resize_hints> Plugin::getResizeHints()
{
  clap_gui_resize_hints resizeHints{};

  if (!plugin.gui)
  {
    return std::nullopt;
  }

  return plugin.gui->get_resize_hints(plugin.plugin, &resizeHints) ? std::make_optional(resizeHints)
                                                                   : std::nullopt;
}

void Plugin::refreshLayout()
{
  settings.repaint();

  settings.api.refreshFont(settings.scale);
  settings.output.refreshFont(settings.scale);
  settings.input.refreshFont(settings.scale);
  settings.sampleRate.refreshFont(settings.scale);
  settings.bufferSize.refreshFont(settings.scale);
  settings.midiIn.refreshFont(settings.scale);

  auto x{static_cast<uint32_t>(150 * settings.scale)};
  auto width{static_cast<uint32_t>(500 * settings.scale)};
  auto height{settings.api.position.height};

  settings.api.setPosition(x, 10, width - x - 10, height);

  settings.output.setPosition(x, settings.api.position.y + settings.api.position.height + 10,
                              width - x - 10, height);

  settings.input.setPosition(x, settings.output.position.y + settings.output.position.height + 10,
                             width - x - 10, height);

  settings.sampleRate.setPosition(x, settings.input.position.y + settings.input.position.height + 10,
                                  width - x - 10, height);

  settings.bufferSize.setPosition(
      x, settings.sampleRate.position.y + settings.sampleRate.position.height + 10, width - x - 10,
      height);

  settings.midiIn.setPosition(
      x, settings.bufferSize.position.y + settings.bufferSize.position.height + 10, width - x - 10,
      (static_cast<uint32_t>(settings.midiIn.getItemHeight() * sah->numMidiPorts)));

  if (sah->numMidiPorts != 0)
  {
    settings.adjustSize(width, (settings.api.position.height * 5) + (10 * 6) +
                                   (settings.midiIn.position.height) + (10 * 1));
  }
  else
  {
    settings.adjustSize(width, (settings.api.position.height * 5) + (10 * 6));
  }
}

void Plugin::refreshApis()
{
  settings.api.reset();

  for (auto& api : sah->getCompiledApi())
  {
    settings.api.add(sah->rtaDac->getApiDisplayName(api));
  }
}

void Plugin::refreshOutputs()
{
  settings.output.reset();

  for (auto& device : sah->getOutputAudioDevices())
  {
    settings.output.add(device.name);
  }
}

void Plugin::refreshInputs()
{
  settings.input.reset();

  for (auto& device : sah->getInputAudioDevices())
  {
    settings.input.add(device.name);
  }
}

void Plugin::refreshSampleRates()
{
  settings.sampleRate.reset();

  auto sampleRates{sah->getSampleRates()};

  for (auto sampleRate : sampleRates)
  {
    settings.sampleRate.add(std::to_string(sampleRate));
  }

  if (!settings.sampleRate.set(std::to_string(sah->currentSampleRate)))
  {
    settings.sampleRate.set(0);
  }
}

void Plugin::refreshBufferSizes()
{
  settings.bufferSize.reset();

  auto bufferSizes{sah->getBufferSizes()};

  for (auto bufferSize : bufferSizes)
  {
    settings.bufferSize.add(std::to_string(bufferSize));
  }

  if (!settings.bufferSize.set(std::to_string(sah->currentBufferSize)))
  {
    settings.bufferSize.set(0);
  }
}

void Plugin::refreshMIDIInputs()
{
  settings.midiIn.reset();

  auto midiIn{std::make_unique<RtMidiIn>()};

  for (uint32_t port{0}; port < sah->numMidiPorts; port++)
  {
    settings.midiIn.add(midiIn->getPortName(port));
  }
}

bool Plugin::saveSettings()
{
  auto settingsPath{getStandaloneSettingsPath()};

  if (settingsPath.has_value())
  {
    std::filesystem::create_directories(settingsPath.value() / plugin.plugin->desc->id);

    settings.set<std::string>("audioApiName", sah->audioApiName);
    settings.set<double>("audioInputDeviceID", sah->audioInputDeviceID);
    settings.set<double>("audioOutputDeviceID", sah->audioOutputDeviceID);
    settings.set<bool>("audioInputUsed", sah->audioInputUsed);
    settings.set<bool>("audioOutputUsed", sah->audioOutputUsed);
    settings.set<double>("currentSampleRate", sah->currentSampleRate);
    settings.set<double>("currentBufferSize", sah->currentBufferSize);
    settings.set<Position>("position", position);

    auto settingsFilePath{settingsPath.value() / plugin.plugin->desc->id / "settings.json"};
    std::wofstream file(settingsFilePath.c_str(), std::ios::binary | std::ios::out);

    if (file.is_open())
    {
      auto serialized{settings.json.Stringify()};
      file.write(serialized.c_str(), serialized.size());

      return file ? true : false;
    }
  }

  return false;
}

bool Plugin::loadSettings()
{
  auto settingsPath{getStandaloneSettingsPath()};

  if (settingsPath.has_value())
  {
    auto settingsFilePath{settingsPath.value() / plugin.plugin->desc->id / "settings.json"};
    std::wifstream file(settingsFilePath.c_str(), std::ios::binary | std::ios::in);

    if (file.is_open())
    {
      std::wstring buffer;
      file.ignore(std::numeric_limits<std::streamsize>::max());
      buffer.resize(file.gcount());
      file.clear();
      file.seekg(0, std::ios::beg);
      file.read(buffer.data(), buffer.size());
      if (auto parsed{settings.json.TryParse(buffer, settings.json)}; parsed)
      {
        sah->audioApiName = settings.get<std::string>("audioApiName");
        sah->audioApi = sah->rtaDac->getCompiledApiByName(sah->audioApiName);
        sah->audioApiDisplayName = sah->rtaDac->getApiDisplayName(sah->audioApi);
        sah->audioInputDeviceID = static_cast<unsigned int>(settings.get<double>("audioInputDeviceID"));
        sah->audioOutputDeviceID =
            static_cast<unsigned int>(settings.get<double>("audioOutputDeviceID"));
        sah->audioInputUsed = settings.get<bool>("audioInputUsed");
        sah->audioOutputUsed = settings.get<bool>("audioOutputUsed");
        sah->currentSampleRate = static_cast<unsigned int>(settings.get<double>("currentSampleRate"));
        sah->currentBufferSize = static_cast<unsigned int>(settings.get<double>("currentBufferSize"));
        position = settings.get<Position>("position");

        return parsed;
      }
    }
  }

  return false;
}

void Plugin::initializeMIDI()
{
  auto midiIn{std::make_unique<RtMidiIn>()};

  sah->currentMidiPorts.clear();

  for (uint32_t port{0}; port < sah->numMidiPorts; port++)
  {
    sah->currentMidiPorts.push_back(port);
  }
}

void Plugin::startMIDI()
{
  sah->startMIDIThread();
}

void Plugin::initializeAudio(RtAudio::Api api)
{
  sah->setAudioApi(api);

  auto [input, output, sampleRate]{sah->getDefaultAudioInOutSampleRate()};

  sah->audioInputDeviceID = input;
  sah->totalInputChannels = sah->rtaDac->getDeviceInfo(input).inputChannels;
  sah->audioInputUsed = true;

  sah->audioOutputDeviceID = output;
  sah->totalOutputChannels = sah->rtaDac->getDeviceInfo(output).outputChannels;
  sah->audioOutputUsed = true;

  sah->currentSampleRate = sampleRate;
  sah->currentBufferSize = sah->getBufferSizes()[0];
}

void Plugin::startAudio()
{
  sah->startAudioThreadOn(sah->audioInputDeviceID, sah->totalInputChannels, sah->audioInputUsed,
                          sah->audioOutputDeviceID, sah->totalOutputChannels, sah->audioOutputUsed,
                          sah->currentSampleRate);
}
}  // namespace freeaudio::clap_wrapper::standalone::windows_standalone
