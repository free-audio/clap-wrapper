#include "windows_standalone.h"

namespace freeaudio::clap_wrapper::standalone::windows_standalone
{
std::vector<std::string> getArgs()
{
  int argc{0};
  wil::unique_hlocal_ptr<wchar_t *[]> buffer;
  buffer.reset(::CommandLineToArgvW(::GetCommandLineW(), &argc));

  std::vector<std::string> argv;

  for (int i = 0; i < argc; i++)
  {
    argv.emplace_back(toUTF8(buffer[i]));
  }

  return argv;
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

bool isRectOnAnyMonitor(const Position &position)
{
  ::RECT rect{position.x, position.y, position.x + static_cast<::LONG>(position.width),
              position.y + static_cast<::LONG>(position.height)};

  // MONITOR_DEFAULTTONULL => null when the rectangle doesn't intersect any display.
  return ::MonitorFromRect(&rect, MONITOR_DEFAULTTONULL) != nullptr;
}

/*
 * The source length and the destination length are different quantities and both
 * of these used to keep them in one variable: it started as the source length,
 * was overwritten with the required destination length by the sizing call, and
 * was then handed to the conversion call as the source length.
 *
 * For toUTF16 that silently truncated any multi-byte input, since the wide count
 * is smaller than the byte count. For toUTF8 it read *past the end of the source*,
 * because the UTF-8 form is longer than the UTF-16 form for anything non-ASCII.
 *
 * These run on the command line, every device name, every system error string,
 * and the settings round trip, so on a non-English Windows they run constantly.
 */
std::wstring toUTF16(std::string_view input)
{
  if (input.empty()) return {};

  if (input.length() > static_cast<size_t>(std::numeric_limits<int>::max()))
  {
    log("toUTF16(): String too long");
    return {};
  }

  auto sourceLength{static_cast<int>(input.length())};

  auto destinationLength{::MultiByteToWideChar(CP_UTF8, 0, input.data(), sourceLength, nullptr, 0)};
  if (destinationLength <= 0)
  {
    log("toUTF16(): {}", getLastError());
    return {};
  }

  std::wstring output(static_cast<size_t>(destinationLength), L'\0');

  if (::MultiByteToWideChar(CP_UTF8, 0, input.data(), sourceLength, output.data(), destinationLength) ==
      0)
  {
    log("toUTF16(): {}", getLastError());
    return {};
  }

  return output;
}

std::string toUTF8(std::wstring_view input)
{
  if (input.empty()) return {};

  if (input.length() > static_cast<size_t>(std::numeric_limits<int>::max()))
  {
    log("toUTF8(): String too long");
    return {};
  }

  auto sourceLength{static_cast<int>(input.length())};

  auto destinationLength{
      ::WideCharToMultiByte(CP_UTF8, 0, input.data(), sourceLength, nullptr, 0, nullptr, nullptr)};
  if (destinationLength <= 0)
  {
    log("toUTF8(): {}", getLastError());
    return {};
  }

  std::string output(static_cast<size_t>(destinationLength), '\0');

  if (::WideCharToMultiByte(CP_UTF8, 0, input.data(), sourceLength, output.data(), destinationLength,
                            nullptr, nullptr) == 0)
  {
    log("toUTF8(): {}", getLastError());
    return {};
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

void log(const std::string &message)
{
  ::OutputDebugStringW(toUTF16(message).c_str());
  ::OutputDebugStringW(L"\n");
}

void log(const std::wstring &message)
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
  // A C++ exception which escapes a window procedure does not unwind - the
  // kernel terminates the process with STATUS_FATAL_USER_CALLBACK_EXCEPTION and
  // no diagnostic at all. These handlers drive audio backends and third-party
  // driver code, so contain what we can here; this is the one place every
  // handler for both windows passes through. Note this catches C++ exceptions
  // only: an access violation inside a driver is a structured exception and is
  // still fatal, by design - there is nothing safe to continue to.
  try
  {
    return map.find(message.msg)->second({message.hwnd, message.msg, message.wparam, message.lparam});
  }
  catch (const std::exception &e)
  {
    log("Unhandled exception in window message {}: {}", message.msg, e.what());
  }
  catch (...)
  {
    log("Unhandled exception in window message {}", message.msg);
  }

  return 0;
}

void MessageHandler::box(const std::string &message)
{
  ::MessageBoxW(nullptr, toUTF16(message).c_str(), nullptr, MB_OK | MB_ICONASTERISK);
}

void MessageHandler::box(const std::wstring &message)
{
  ::MessageBoxW(nullptr, message.c_str(), nullptr, MB_OK | MB_ICONASTERISK);
}

void MessageHandler::error(const std::string &message)
{
  ::MessageBoxW(nullptr, toUTF16(message).c_str(), nullptr, MB_OK | MB_ICONHAND);
}

void MessageHandler::error(const std::wstring &message)
{
  ::MessageBoxW(nullptr, message.c_str(), nullptr, MB_OK | MB_ICONHAND);
}

void Window::create(const std::string &title)
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
  windowClass.cbWndExtra = sizeof(void *);
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
    auto create{reinterpret_cast<::CREATESTRUCTW *>(lparam)};

    if (auto self{static_cast<Window *>(create->lpCreateParams)}; self)
    {
      ::SetWindowLongPtrW(hwnd, 0, reinterpret_cast<::LONG_PTR>(self));
      self->hwnd.reset(hwnd);
      self->dpi = static_cast<uint32_t>(::GetDpiForWindow(hwnd));
      self->scale = (static_cast<double>(self->dpi) / static_cast<double>(USER_DEFAULT_SCREEN_DPI));
    }
  }

  if (auto self{reinterpret_cast<Window *>(::GetWindowLongPtrW(hwnd, 0))}; self)
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

void ComboBox::create(const std::string &name, uintptr_t id, ::HWND parentHwnd)
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

void ComboBox::add(const std::string &string)
{
  message.send(hwnd.get(), CB_ADDSTRING, 0, toUTF16(string).c_str());
}

bool ComboBox::set(int index)
{
  return message.send(hwnd.get(), CB_SETCURSEL, index, 0) != CB_ERR ? true : false;
}

bool ComboBox::set(const std::string &searchString)
{
  return message.send(hwnd.get(), CB_SELECTSTRING, -1, toUTF16(searchString).c_str()) != CB_ERR ? true
                                                                                                : false;
}

::LRESULT ComboBox::get()
{
  return message.send(hwnd.get(), CB_GETCURSEL, 0, 0);
}

std::optional<size_t> ComboBox::selection(size_t containerSize)
{
  auto index{get()};

  if (index == CB_ERR || index < 0) return std::nullopt;
  if (static_cast<size_t>(index) >= containerSize) return std::nullopt;

  return static_cast<size_t>(index);
}

::LRESULT ComboBox::getItemHeight()
{
  return message.send(hwnd.get(), CB_GETITEMHEIGHT, 0, 0);
}

::LRESULT CALLBACK ComboBox::procedure(::HWND hwnd, ::UINT msg, ::WPARAM wparam, ::LPARAM lparam,
                                       ::UINT_PTR id, ::DWORD_PTR data)
{
  if (auto self{reinterpret_cast<ComboBox *>(data)}; self)
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

void ListBox::create(const std::string &name, uintptr_t id, ::HWND parentHwnd)
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

void ListBox::add(const std::string &string)
{
  message.send(hwnd.get(), LB_ADDSTRING, 0, toUTF16(string).c_str());
}

// This is an LBS_MULTIPLESEL listbox, so the single-selection messages don't
// apply to it: LB_SETCURSEL is documented as having no effect on a multi-select
// listbox, and LB_GETCURSEL is not the way to read one. Using them meant a
// restored multi-port MIDI selection could never be shown.
bool ListBox::set(int index)
{
  return message.send(hwnd.get(), LB_SETSEL, TRUE, index) != LB_ERR;
}

bool ListBox::set(const std::string &searchString)
{
  auto index{message.send(hwnd.get(), LB_FINDSTRINGEXACT, -1, toUTF16(searchString).c_str())};

  if (index == LB_ERR) return false;

  return set(static_cast<int>(index));
}

void ListBox::clearSelection()
{
  message.send(hwnd.get(), LB_SETSEL, FALSE, -1);
}

::LRESULT ListBox::get()
{
  return message.send(hwnd.get(), LB_GETCURSEL, 0, 0);
}

::LRESULT ListBox::getItems(std::vector<int> &buffer)
{
  auto count{getItemsCount()};

  // LB_GETSELCOUNT answers LB_ERR (-1) on a single-selection listbox; resizing
  // to that would be an enormous allocation rather than an empty list.
  if (count == LB_ERR || count <= 0)
  {
    buffer.clear();
    return 0;
  }

  buffer.resize(static_cast<size_t>(count));

  return message.send(hwnd.get(), LB_GETSELITEMS, count, buffer.data());
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
  if (auto self{reinterpret_cast<ComboBox *>(data)}; self)
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

void SystemMenu::add(std::wstring &name, ::UINT id)
{
  item.emplace_back(::MENUITEMINFOW{sizeof(::MENUITEMINFOW), MIIM_STRING | MIIM_ID, 0, 0, id, nullptr,
                                    nullptr, nullptr, 0, name.data(), 0, nullptr});
}

void SystemMenu::addToggle(std::wstring &name, ::UINT id, bool checked)
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

Plugin::Plugin(std::shared_ptr<Clap::Plugin> clapPlugin, int nCmdShow)
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

                 // Never push a degenerate size into the plugin GUI. A minimized window has a
                 // 0x0 client rect; resizing the editor to that drives size-dependent drawing
                 // to zero dimensions.
                 if (!::IsIconic(msg.hwnd) && client.width > 0 && client.height > 0 &&
                     plugin.gui->can_resize(plugin.plugin))
                 {
                   plugin.gui->adjust_size(plugin.plugin, &client.width, &client.height);
                   plugin.gui->set_size(plugin.plugin, client.width, client.height);
                 }
               }

               // Only remember the restored (normal) geometry. Capturing while minimized or
               // maximized would persist the -32000 iconic sentinel and throw the window
               // off-screen on the next launch. GetWindowRect is the true screen rect (the
               // WINDOWPOS x/y are unreliable when SWP_NOMOVE is set).
               if (!::IsIconic(msg.hwnd) && !::IsZoomed(msg.hwnd))
               {
                 ::RECT wr{};
                 ::GetWindowRect(msg.hwnd, &wr);
                 position.x = wr.left;
                 position.y = wr.top;
                 position.width = static_cast<uint32_t>(wr.right - wr.left);
                 position.height = static_cast<uint32_t>(wr.bottom - wr.top);
               }

               return 0;
             });

  message.on(WM_EXITSIZEMOVE,
             [this](Message msg)
             {
               // Persist once a user move/resize finishes rather than on every intermediate
               // WM_WINDOWPOSCHANGED. (Close also saves via WM_DESTROY.)
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
               if (!sah) return 0;

               switch (msg.wparam)
               {
                 case Menu::Identifier::AudioMidiSettings:
                 {
                   settings.isVisible() ? settings.hide() : settings.show();

                   return 0;
                 }

                 case Menu::Identifier::MuteInput:
                 {
                   // The one place the user states input intent, so persist it here.
                   if (menu.item[1].fState == MFS_UNCHECKED)
                   {
                     sah->audioInputUsed = false;
                     sah->settings.audioInputUsed = false;
                     menu.item[1].fState = MFS_CHECKED;
                   }
                   else
                   {
                     sah->audioInputUsed = true;
                     sah->settings.audioInputUsed = true;
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

                     auto saveFile{fs::path(result.get())};

                     try
                     {
                       sah->saveStandaloneAndPluginSettings(saveFile.parent_path(), saveFile.filename());
                     }
                     catch (const fs::filesystem_error &e)
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

                     auto saveFile{fs::path(result.get())};

                     try
                     {
                       if (fs::exists(saveFile))
                       {
                         sah->tryLoadStandaloneAndPluginSettings(saveFile.parent_path(),
                                                                 saveFile.filename());
                       }
                     }
                     catch (const fs::filesystem_error &e)
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
                     catch (const fs::filesystem_error &e)
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

  settings.message.on(WM_COMMAND,
                      [this](Message msg)
                      {
                        if (!sah) return 0;

                        if (HIWORD(msg.wparam) == CBN_SELCHANGE)
                        {
                          // Every one of these reads a combo selection and uses it as a vector
                          // index. CB_GETCURSEL returns -1 when nothing is selected, so each was
                          // an out-of-bounds read waiting for an empty or freshly-rebuilt combo.
                          // selection() yields nothing unless the index is real and in range.
                          if (LOWORD(msg.wparam) == Settings::Identifier::AudioApi)
                          {
                            auto apis{sah->getCompiledApi()};

                            if (auto index{settings.api.selection(apis.size())}; index)
                            {
                              selectAudioApi(apis[*index]);

                              refreshOutputs();
                              refreshInputs();

                              settings.output.set(sah->deviceName(sah->audioOutputDeviceID));
                              settings.input.set(sah->deviceName(sah->audioInputDeviceID));

                              refreshSampleRates();
                              refreshBufferSizes();

                              settings.sampleRate.set(std::to_string(sah->currentSampleRate));
                              settings.bufferSize.set(std::to_string(sah->currentBufferSize));

                              saveSettings();
                              startAudio();
                            }
                          }

                          if (LOWORD(msg.wparam) == Settings::Identifier::AudioOutput)
                          {
                            auto devices{sah->getOutputAudioDevices()};

                            if (auto index{settings.output.selection(devices.size())}; index)
                            {
                              sah->audioOutputDeviceID = devices[*index].ID;
                              sah->deviceOutputChannels = devices[*index].outputChannels;
                              sah->audioOutputUsed = true;

                              // Persist the choice here; the device actually
                              // opened may be a fallback.
                              sah->settings.outputDeviceName = devices[*index].name;
                              sah->settings.audioOutputUsed = true;

                              refreshSampleRates();
                              refreshBufferSizes();

                              saveSettings();
                              startAudio();
                            }
                          }

                          if (LOWORD(msg.wparam) == Settings::Identifier::AudioInput)
                          {
                            auto devices{sah->getInputAudioDevices()};

                            if (auto index{settings.input.selection(devices.size())}; index)
                            {
                              sah->audioInputDeviceID = devices[*index].ID;
                              sah->deviceInputChannels = devices[*index].inputChannels;
                              sah->audioInputUsed = true;

                              // Picking an input device unmutes it.
                              sah->settings.inputDeviceName = devices[*index].name;
                              sah->settings.audioInputUsed = true;

                              refreshSampleRates();
                              refreshBufferSizes();

                              saveSettings();
                              startAudio();
                            }
                          }

                          if (LOWORD(msg.wparam) == Settings::Identifier::AudioSamplerate)
                          {
                            // This used to take the rate list from the *input* device vector,
                            // indexed by the input combo - so picking a sample rate read out of
                            // bounds whenever the current API had no input devices at all, which
                            // is routine for output-only ASIO drivers. The rates offered are the
                            // ones the stream can actually run at.
                            auto sampleRates{sah->getSampleRates()};

                            if (auto index{settings.sampleRate.selection(sampleRates.size())}; index)
                            {
                              sah->currentSampleRate = sampleRates[*index];
                              sah->settings.sampleRate = sampleRates[*index];

                              saveSettings();
                              startAudio();
                            }
                          }

                          if (LOWORD(msg.wparam) == Settings::Identifier::AudioBuffersize)
                          {
                            auto bufferSizes{sah->getBufferSizes()};

                            if (auto index{settings.bufferSize.selection(bufferSizes.size())}; index)
                            {
                              sah->currentBufferSize = bufferSizes[*index];

                              saveSettings();
                              startAudio();
                            }
                          }
                        }

                        if (HIWORD(msg.wparam) == LBN_SELCHANGE)
                        {
                          if (LOWORD(msg.wparam) == Settings::Identifier::MidiInputs)
                          {
                            std::vector<int> selected;
                            settings.midiIn.getItems(selected);

                            auto available{sah->getMidiPortNames()};

                            std::vector<std::string> chosen;
                            for (auto index : selected)
                            {
                              if (index >= 0 && static_cast<size_t>(index) < available.size())
                              {
                                chosen.push_back(available[index]);
                              }
                            }

                            sah->openMidiPorts(chosen, false);

                            // Record names, not indices: the index of a port changes as devices
                            // come and go. Once the user has touched the list we stop binding
                            // everything, so deselecting every port really does mean no MIDI in.
                            sah->settings.midiBindAllPorts = false;
                            sah->settings.midiPortNames = chosen;

                            saveSettings();
                          }
                        }

                        return 0;
                      });

  message.on(WM_TIMER,
             [this](Message msg)
             {
               if (!sah) return 0;

               if (msg.wparam == timerId)
               {
                 if (sah->callbackRequested.exchange(false))
                 {
                   plugin.plugin->on_main_thread(plugin.plugin);
                 }

                 if (sah->restartRequested.exchange(false))
                 {
                   // Reactivate in place rather than bouncing the whole audio
                   // engine. activatePlugin parks the callback, reactivates, and
                   // only then lets processing resume - and leaves it stopped if
                   // the plugin refuses to reactivate.
                   sah->activatePlugin(sah->currentSampleRate, 1, sah->currentBufferSize * 2);
                 }

                 if (sah->processRequested.exchange(false))
                 {
                   // Unlike a plugin wrapper, the standalone owns its audio
                   // engine, so a wake request means something here: if the
                   // plugin is not active, stand it up. If it already is, the
                   // callback is pulling it and there is nothing to do. The
                   // sample rate check keeps this from firing before the
                   // engine has ever run.
                   if (!sah->isActive && sah->currentSampleRate > 0)
                   {
                     sah->activatePlugin(sah->currentSampleRate, 1, sah->currentBufferSize * 2);
                   }
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
                 // KillTimer returns nonzero on success, so this used to log the
                 // last error precisely when nothing had gone wrong.
                 if (!stopTimer(timerId))
                 {
                   log(getLastError());
                 }
                 isTimerRunning = false;
               }

               // The settings window can still be handed messages after the host
               // is gone, and every one of its handlers reads sah. Take it down
               // while it is still safe to do so.
               settings.hwnd.reset();

               // Release our reference to the CLAP *before* mainFinish, which
               // runs entry->deinit(). Holding it here meant the plugin object
               // was destroyed after the entry it came from had been deinited -
               // a spec violation, and a use-after-free of the library in the
               // dynamically loaded case.
               plugin.gui = nullptr;
               plugin.state = nullptr;
               plugin.plugin = nullptr;
               plugin.clap.reset();

               freeaudio::clap_wrapper::standalone::mainFinish();

               // mainFinish destroys the StandaloneHost, so this pointer is dead
               // from here on.
               sah = nullptr;

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
        if (!sah) return 0;

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

  // Exactly one of these. The previous code called loadSettings() and then
  // initializeAudio() unconditionally, and initializeAudio reset the device ids,
  // mute state, sample rate and buffer size to hardware defaults - so everything
  // that had just been restored was thrown away before it was ever used, and only
  // the API name survived to the next launch.
  if (loadSettings())
  {
    applyLoadedAudio();
  }
  else
  {
    applyDefaultAudio();
    saveSettings();
  }

  // Show what the user chose, not the runtime flag, which is also false when
  // there is no capture device at all.
  menu.item[1].fState = sah->settings.audioInputUsed ? MFS_UNCHECKED : MFS_CHECKED;
  SetMenuItemInfoW(getSystemMenu(hwnd.get()), 1, FALSE, &menu.item[1]);

  if (plugin.gui)
  {
    if (plugin.gui->is_api_supported(plugin.plugin, CLAP_WINDOW_API_WIN32, false))
    {
      plugin.gui->create(plugin.plugin, CLAP_WINDOW_API_WIN32, false);
      plugin.gui->set_scale(plugin.plugin, scale);

      if (plugin.gui->can_resize(plugin.plugin))
      {
        // Only restore a saved position if it still lands on a connected monitor. Guards
        // against off-screen/garbage values (e.g. a minimized window's -32000 sentinel
        // persisted by an older build, or a monitor that's no longer attached).
        if ((position.width != 0 || position.height != 0) && isRectOnAnyMonitor(position))
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

      clap_window clapWindow{CLAP_WINDOW_API_WIN32, static_cast<void *>(hwnd.get())};
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
    if (placement.showCmd != SW_MAXIMIZE && placement.showCmd != SW_SHOWMINIMIZED &&
        !::IsIconic(hwnd.get()))
    {
      adjustSize(width, height);
    }

    return true;
  };

  startMIDI();
  refreshMIDIInputs();

  // Never put the message box up from here. RtAudio calls this from wherever the
  // failure happened - including from inside the combo-box handler that asked for
  // the device change, and from its own stream threads. A modal box runs a nested
  // message loop, so showing one there re-enters the handler that is still on the
  // stack. Queue the text and let the main loop show it once it is idle.
  sah->displayAudioError = [this](auto &errorText)
  {
    {
      std::lock_guard<std::mutex> guard(pendingErrorLock);

      // Repeats are the norm: one failure typically reports through several
      // layers, and nobody wants that dialog twice.
      if (pendingError == errorText || errorText == lastShownError) return;
      pendingError = errorText;
    }

    ::PostMessageW(hwnd.get(), WM_SHOW_AUDIO_ERROR, 0, 0);
  };

  message.on(WM_SHOW_AUDIO_ERROR,
             [this](Message msg)
             {
               std::string text;
               {
                 std::lock_guard<std::mutex> guard(pendingErrorLock);
                 text = pendingError;
                 pendingError.clear();
                 lastShownError = text;
               }

               if (!text.empty()) message.error("Unable to configure audio: {}", text);

               return 0;
             });

  refreshApis();
  refreshOutputs();
  refreshInputs();
  refreshSampleRates();
  refreshBufferSizes();

  settings.api.set(sah->audioApiDisplayName);
  // deviceName() looks the id up through the device enumeration. Calling
  // getDeviceInfo() with an id RtAudio doesn't recognise raises an error through
  // the error callback, which by this point is wired to a modal message box -
  // so a perfectly normal launch could greet the user with an error dialog.
  settings.output.set(sah->deviceName(sah->audioOutputDeviceID));
  settings.input.set(sah->deviceName(sah->audioInputDeviceID));
  settings.sampleRate.set(std::to_string(sah->currentSampleRate));
  settings.bufferSize.set(std::to_string(sah->currentBufferSize));

  refreshLayout();

  startAudio();

  // The rate asked for may have been 0 ("device preferred") or unsupported, so the
  // combo can only show the truth once the stream is open.
  refreshSampleRates();

  // Honor the show state requested by the launcher (shortcut "Run:" / STARTUPINFO),
  // falling back to a normal window. SW_HIDE would otherwise leave us invisible-but-running.
  ::ShowWindow(hwnd.get(), nCmdShow == SW_HIDE ? SW_SHOWNORMAL : nCmdShow);
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
  if (!sah) return;

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

  for (auto &api : sah->getCompiledApi())
  {
    settings.api.add(RtAudio::getApiDisplayName(api));
  }
}

void Plugin::refreshOutputs()
{
  settings.output.reset();

  for (auto &device : sah->getOutputAudioDevices())
  {
    settings.output.add(device.name);
  }
}

void Plugin::refreshInputs()
{
  settings.input.reset();

  for (auto &device : sah->getInputAudioDevices())
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

  auto available{sah->getMidiPortNames()};

  for (const auto &name : available)
  {
    settings.midiIn.add(name);
  }

  // Show what is actually open. The list used to be populated with nothing
  // selected while every port was in fact bound, so the UI told the user the
  // opposite of what was happening.
  settings.midiIn.clearSelection();

  if (sah->settings.midiBindAllPorts)
  {
    for (int index{0}; index < static_cast<int>(available.size()); ++index)
    {
      settings.midiIn.set(index);
    }
  }
  else
  {
    for (const auto &name : sah->settings.midiPortNames)
    {
      settings.midiIn.set(name);
    }
  }
}

bool Plugin::saveSettings()
{
  if (!sah) return false;

  sah->captureAudioSettings();

  // Only offer a geometry back to the next launch once we have a real one.
  sah->settings.hasWindowPosition = (position.width != 0 && position.height != 0);
  sah->settings.windowX = position.x;
  sah->settings.windowY = position.y;
  sah->settings.windowWidth = position.width;
  sah->settings.windowHeight = position.height;

  return sah->saveStandaloneSettings();
}

bool Plugin::loadSettings()
{
  if (!sah || !sah->loadStandaloneSettings()) return false;

  if (sah->settings.hasWindowPosition)
  {
    position.x = sah->settings.windowX;
    position.y = sah->settings.windowY;
    position.width = sah->settings.windowWidth;
    position.height = sah->settings.windowHeight;
  }

  return true;
}

void Plugin::startMIDI()
{
  sah->startMIDIThread();
}

void Plugin::refreshDeviceChannelCounts()
{
  // How many channels to open the device with. Kept separate from the plugin's
  // bus totals, which is what totalInput/OutputChannels means in the host.
  for (const auto &device : sah->getOutputAudioDevices())
  {
    if (device.ID == sah->audioOutputDeviceID) sah->deviceOutputChannels = device.outputChannels;
  }

  for (const auto &device : sah->getInputAudioDevices())
  {
    if (device.ID == sah->audioInputDeviceID) sah->deviceInputChannels = device.inputChannels;
  }
}

void Plugin::selectDefaultDevices()
{
  auto [input, output, sampleRate]{sah->getDefaultAudioInOutSampleRate()};

  // No device chosen under this API; empty means "follow the system default".
  // Names from the previous API would not resolve here anyway.
  sah->settings.inputDeviceName.clear();
  sah->settings.outputDeviceName.clear();

  // RtAudio hands back a default input device id even on a machine with no
  // capture device at all, so take the id only if it names something real.
  // The probe is a fact about the machine, not a choice: AND it with the user's
  // wish, which survives an API switch, and never persist it.
  sah->audioInputDeviceID = input;
  sah->audioInputUsed = sah->settings.audioInputUsed && sah->isKnownDevice(input);

  sah->audioOutputDeviceID = output;
  sah->audioOutputUsed = sah->settings.audioOutputUsed && sah->isKnownDevice(output);

  sah->currentSampleRate = sampleRate;
  sah->currentBufferSize = StandaloneSettings::defaultBufferSize;

  refreshDeviceChannelCounts();
}

void Plugin::applyLoadedAudio()
{
  sah->applyAudioSettings();
  refreshDeviceChannelCounts();
}

void Plugin::applyDefaultAudio()
{
  // WASAPI is the backend that is always present and always works on a stock
  // Windows, so it is what a first run gets.
  sah->setAudioApi(RtAudio::Api::WINDOWS_WASAPI);
  selectDefaultDevices();
}

void Plugin::selectAudioApi(RtAudio::Api api)
{
  sah->setAudioApi(api);
  selectDefaultDevices();
}

void Plugin::startAudio()
{
  sah->startAudioThreadOn(sah->audioInputDeviceID, sah->deviceInputChannels, sah->audioInputUsed,
                          sah->audioOutputDeviceID, sah->deviceOutputChannels, sah->audioOutputUsed,
                          sah->currentSampleRate);
}
}  // namespace freeaudio::clap_wrapper::standalone::windows_standalone
