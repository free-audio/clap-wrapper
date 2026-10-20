#include "windows_standalone.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace freeaudio::clap_wrapper::standalone::windows_standalone
{
namespace
{
// the plugin window whose settings window the standalone features extension shows;
// null before it is created and after it is destroyed
Plugin *settingsWindowOwner{nullptr};
}  // namespace

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

::HFONT getMessageFont(uint32_t dpi, int percent, int weight)
{
  ::NONCLIENTMETRICSW metrics{sizeof(::NONCLIENTMETRICSW)};
  if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, dpi))
  {
    // DEFAULT_GUI_FONT is MS Sans Serif, which no dialog has used since Windows XP
    ::GetObjectW(getFontFromSystem(), sizeof(metrics.lfMessageFont), &metrics.lfMessageFont);
  }

  auto font{metrics.lfMessageFont};
  font.lfHeight = ::MulDiv(font.lfHeight, percent, 100);
  if (weight != 0) font.lfWeight = weight;

  return ::CreateFontIndirectW(&font);
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

    // the settings window's keyboard: Tab between its controls, Enter for OK, Esc for Cancel
    else if (settingsWindowOwner && settingsWindowOwner->settings.hwnd &&
             ::IsDialogMessageW(settingsWindowOwner->settings.hwnd.get(), &msg))
    {
      continue;
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
  ::MessageBoxW(owner, toUTF16(message).c_str(), nullptr, MB_OK | MB_ICONASTERISK);
}

void MessageHandler::box(const std::wstring &message)
{
  ::MessageBoxW(owner, message.c_str(), nullptr, MB_OK | MB_ICONASTERISK);
}

void MessageHandler::error(const std::string &message)
{
  ::MessageBoxW(owner, toUTF16(message).c_str(), nullptr, MB_OK | MB_ICONHAND);
}

void MessageHandler::error(const std::wstring &message)
{
  ::MessageBoxW(owner, message.c_str(), nullptr, MB_OK | MB_ICONHAND);
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
      self->suggested.width = rect->right - rect->left;
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

void Control::create(::LPCWSTR className, const std::string &text, ::DWORD style, uintptr_t id,
                     ::HWND parentHwnd, ::DWORD exStyle)
{
  hwnd.reset(::CreateWindowExW(exStyle, className, toUTF16(text).c_str(), WS_CHILD | WS_VISIBLE | style,
                               0, 0, 0, 0, parentHwnd, (::HMENU)id, getInstance(), nullptr));
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

void Control::setFont(::HFONT font)
{
  message.send(hwnd.get(), WM_SETFONT, font, TRUE);
}

void Control::setText(const std::string &text)
{
  ::SetWindowTextW(hwnd.get(), toUTF16(text).c_str());
}

void Control::setVisible(bool show)
{
  visible = show;
  ::ShowWindow(hwnd.get(), show ? SW_SHOW : SW_HIDE);
}

void ComboBox::create(const std::string &name, uintptr_t id, ::HWND parentHwnd)
{
  Control::create(WC_COMBOBOXW, name, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id, parentHwnd);

  ::SetWindowSubclass(hwnd.get(), &procedure, id, reinterpret_cast<::DWORD_PTR>(this));
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

void PortList::create(uintptr_t id, ::HWND parentHwnd)
{
  Control::create(WC_LISTVIEWW, "",
                  LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, id,
                  parentHwnd, WS_EX_CLIENTEDGE);

  ListView_SetExtendedListViewStyleEx(hwnd.get(), LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER,
                                      LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);

  ::LVCOLUMNW column{};
  column.mask = LVCF_WIDTH;
  column.cx = 100;
  ListView_InsertColumn(hwnd.get(), 0, &column);

  empty.create(WC_STATICW, "", SS_LEFT | SS_ENDELLIPSIS | SS_CENTERIMAGE | SS_NOPREFIX,
               Plugin::Settings::Identifier::Static, parentHwnd, WS_EX_CLIENTEDGE);
}

namespace
{
bool contains(const std::vector<std::string> &names, const std::string &name)
{
  return std::find(names.begin(), names.end(), name) != names.end();
}
}  // namespace

void PortList::fill(const std::vector<MidiPort> &ports, const std::vector<std::string> &onIds,
                    const std::vector<std::string> &absentNames, bool allOn)
{
  // what the rows there now show, which a port coming or going does not change
  std::vector<std::pair<Row, bool>> previous;
  for (size_t i = 0; i < rows.size(); ++i) previous.emplace_back(rows[i], isOn(i));

  ListView_DeleteAllItems(hwnd.get());
  rows.clear();

  auto addRow =
      [this, &previous](const std::string &reference, const std::string &name, bool connected, bool isOn)
  {
    bool wasOn{isOn};
    for (const auto &[row, on] : previous)
    {
      if (row.reference != reference) continue;
      isOn = on;
      wasOn = row.wasOn;
    }

    auto title{toUTF16(name)};
    if (!connected) title += L" (not connected)";

    ::LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(rows.size());
    item.pszText = title.data();
    ListView_InsertItem(hwnd.get(), &item);

    rows.push_back({reference, wasOn});
    setOn(rows.size() - 1, isOn);
  };

  for (const auto &port : ports) addRow(port.id, port.name, true, allOn || contains(onIds, port.id));
  if (!allOn)
    for (const auto &name : absentNames) addRow(name, name, false, true);
}

bool PortList::isOn(size_t row)
{
  return ListView_GetCheckState(hwnd.get(), static_cast<int>(row)) != FALSE;
}

void PortList::setOn(size_t row, bool on)
{
  ListView_SetCheckState(hwnd.get(), static_cast<int>(row), on ? TRUE : FALSE);
}

bool PortList::changed()
{
  for (size_t i = 0; i < rows.size(); ++i)
    if (isOn(i) != rows[i].wasOn) return true;
  return false;
}

std::vector<std::string> PortList::ticked()
{
  std::vector<std::string> references;
  for (size_t i = 0; i < rows.size(); ++i)
    if (isOn(i)) references.push_back(rows[i].reference);
  return references;
}

void PortList::setTheme(bool dark, ::COLORREF background, ::COLORREF text, ::COLORREF frame)
{
  ::SetWindowTheme(hwnd.get(), dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  ListView_SetBkColor(hwnd.get(), background);
  ListView_SetTextBkColor(hwnd.get(), background);
  ListView_SetTextColor(hwnd.get(), text);

  // turning the checkboxes off and on again makes their images in the new theme, and
  // loses the ticks, so they are put back
  std::vector<bool> on;
  for (size_t i = 0; i < rows.size(); ++i) on.push_back(isOn(i));
  ListView_SetExtendedListViewStyleEx(hwnd.get(), LVS_EX_CHECKBOXES, 0);
  ListView_SetExtendedListViewStyleEx(hwnd.get(), LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);

  // a list view draws its checkboxes from the light button theme whatever its own, and
  // the dark button theme has no dark checkbox on windows 10 either, so in the dark the
  // two images, unticked and ticked, are drawn here
  if (dark)
  {
    const auto dpi{::GetDpiForWindow(hwnd.get())};
    const int cell{::MulDiv(16, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
    const int box{::MulDiv(13, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)};
    const int inset{(cell - box) / 2};
    const int line{std::max(1, ::MulDiv(1, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI))};

    auto images{::ImageList_Create(cell, cell, ILC_COLOR32, 2, 0)};
    auto dc{::GetDC(hwnd.get())};
    wil::unique_hbrush fill{::CreateSolidBrush(background)};
    wil::unique_hbrush border{::CreateSolidBrush(frame)};
    wil::unique_hpen tick{::CreatePen(PS_SOLID, std::max(2, box / 7), text)};

    for (bool ticked : {false, true})
    {
      wil::unique_hbitmap bitmap{::CreateCompatibleBitmap(dc, cell, cell)};
      auto memory{::CreateCompatibleDC(dc)};
      auto previous{::SelectObject(memory, bitmap.get())};

      ::RECT rect{0, 0, cell, cell};
      ::FillRect(memory, &rect, fill.get());

      // the box, as a frame line wide
      ::RECT frameRect{inset, inset, inset + box, inset + box};
      for (int i = 0; i < line; ++i)
      {
        ::FrameRect(memory, &frameRect, border.get());
        ::InflateRect(&frameRect, -1, -1);
      }

      if (ticked)
      {
        const ::POINT points[]{{inset + box * 22 / 100, inset + box * 52 / 100},
                               {inset + box * 42 / 100, inset + box * 72 / 100},
                               {inset + box * 78 / 100, inset + box * 30 / 100}};
        auto previousPen{::SelectObject(memory, tick.get())};
        ::Polyline(memory, points, 3);
        ::SelectObject(memory, previousPen);
      }

      ::SelectObject(memory, previous);
      ::DeleteDC(memory);
      ::ImageList_Add(images, bitmap.get(), nullptr);
    }

    ::ReleaseDC(hwnd.get(), dc);

    // the list owns the new images, and gives back the ones it made itself
    if (auto old{ListView_SetImageList(hwnd.get(), images, LVSIL_STATE)}; old) ::ImageList_Destroy(old);
  }

  for (size_t i = 0; i < on.size(); ++i) setOn(i, on[i]);

  ::SetWindowTheme(empty.hwnd.get(), dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

uint32_t PortList::rowHeight()
{
  // the list answers from its font, and only for a row it has
  ::RECT rect{};
  if (rows.empty() || !ListView_GetItemRect(hwnd.get(), 0, &rect, LVIR_BOUNDS)) return 0;
  return static_cast<uint32_t>(rect.bottom - rect.top);
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

bool showSettingsScreen()
{
  if (!settingsWindowOwner) return false;

  settingsWindowOwner->showSettings();
  return true;
}

namespace
{
// the settings window's measures, in device independent pixels; the macos window's, in
// points, where windows has no convention of its own
constexpr int windowMargin{20};
constexpr int headingTop{16};
constexpr int rowSpacing{8};
constexpr int columnSpacing{8};
constexpr int rulePadding{6};
constexpr int controlWidth{320};
constexpr int smallControlWidth{120};
constexpr int buttonHeight{23};
constexpr int buttonMinWidth{80};
constexpr int buttonPadding{12};
constexpr int buttonSpacing{8};
constexpr int buttonsTop{24};
constexpr int dropDownHeight{300};
// port lists show this many rows and scroll the rest
constexpr size_t visiblePortRows{5};

// whether "auto" means dark: the apps setting in Personalization > Colors
bool systemPrefersDark()
{
  ::DWORD value{1};
  ::DWORD size{sizeof(value)};
  if (::RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
  {
    return false;
  }
  return value == 0;
}

bool isDark(uint32_t appearance)
{
  switch (appearance)
  {
    case CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK:
      return true;
    case CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT:
      return false;
    default:
      return systemPrefersDark();
  }
}

Plugin::Palette paletteFor(bool dark)
{
  if (dark)
  {
    return {true,
            RGB(32, 32, 32),
            RGB(255, 255, 255),
            RGB(160, 160, 160),
            RGB(255, 153, 164),
            RGB(255, 159, 10),
            RGB(70, 70, 70),
            RGB(43, 43, 43)};
  }

  return {false,
          ::GetSysColor(COLOR_BTNFACE),
          ::GetSysColor(COLOR_BTNTEXT),
          ::GetSysColor(COLOR_GRAYTEXT),
          RGB(196, 43, 28),
          RGB(202, 80, 16),
          RGB(204, 204, 204),
          ::GetSysColor(COLOR_WINDOW)};
}

std::wstring windowText(::HWND hwnd)
{
  std::wstring text(static_cast<size_t>(::GetWindowTextLengthW(hwnd)) + 1, L'\0');
  text.resize(static_cast<size_t>(::GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()))));
  return text;
}

// the size of one line of a control's text, in its font
::SIZE textExtent(::HFONT font, ::HWND hwnd)
{
  const auto text{windowText(hwnd)};
  ::SIZE size{};

  auto dc{::GetDC(nullptr)};
  auto previous{::SelectObject(dc, font)};
  ::GetTextExtentPoint32W(dc, text.empty() ? L"M" : text.c_str(),
                          text.empty() ? 1 : static_cast<int>(text.size()), &size);
  if (text.empty()) size.cx = 0;
  ::SelectObject(dc, previous);
  ::ReleaseDC(nullptr, dc);

  return size;
}

// the height a control's text takes, wrapped at that width
int wrappedHeight(::HFONT font, ::HWND hwnd, int width)
{
  auto text{windowText(hwnd)};
  ::RECT rect{0, 0, width, 0};

  auto dc{::GetDC(nullptr)};
  auto previous{::SelectObject(dc, font)};
  ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  ::SelectObject(dc, previous);
  ::ReleaseDC(nullptr, dc);

  return rect.bottom - rect.top;
}

std::string pluginDisplayName(const clap_plugin_t *plugin)
{
  if (plugin && plugin->desc && plugin->desc->name && *plugin->desc->name) return plugin->desc->name;
  return OUTPUT_NAME;
}
}  // namespace

void Plugin::showSettings()
{
  if (!settings.hwnd || !sah) return;

  // a new showing: built from what runs now, as macOS builds a new window
  if (!settings.isVisible())
  {
    settingsApplied = false;
    populateSettings();
    layoutSettings();

    // centred on the screen the plugin window is on. moving there may change the dpi,
    // and with it the size, so it is centred again once it has arrived
    auto centre = [this]()
    {
      ::MONITORINFO mi{sizeof(::MONITORINFO)};
      ::GetMonitorInfoW(::MonitorFromWindow(hwnd.get(), MONITOR_DEFAULTTONEAREST), &mi);

      ::RECT rect{};
      ::GetWindowRect(settings.hwnd.get(), &rect);

      const auto width{rect.right - rect.left};
      const auto height{rect.bottom - rect.top};
      const auto x{mi.rcWork.left + std::max(0L, (mi.rcWork.right - mi.rcWork.left - width) / 2)};
      const auto y{mi.rcWork.top + std::max(0L, (mi.rcWork.bottom - mi.rcWork.top - height) / 2)};

      ::SetWindowPos(settings.hwnd.get(), nullptr, x, y, 0, 0,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
    };
    centre();
    centre();

    settings.show();
  }

  ::SetForegroundWindow(settings.hwnd.get());
}

void Plugin::hideSettings()
{
  if (!settings.isVisible()) return;

  settings.hide();
  ::PostMessageW(hwnd.get(), WM_SETTINGS_CLOSED, settingsApplied ? 1 : 0, 0);
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
               menu.addSeparator();
               menu.add(menu.saveState, Menu::Identifier::SaveState);
               menu.add(menu.loadState, Menu::Identifier::LoadState);
               menu.add(menu.resetState, Menu::Identifier::ResetState);
               menu.addSeparator();
               menu.addToggle(menu.restoreLastSession, Menu::Identifier::RestoreLastSession,
                              sah && sah->restoresLastSession());
               menu.addSeparator();

               menu.populate(hwnd.get());

               // an error box over this window, never behind it
               message.owner = msg.hwnd;

               return 0;
             });

  settings.message.on(WM_CREATE,
                      [this](Message msg)
                      {
                        settings.message.owner = msg.hwnd;
                        settings.setStyle(WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX);
                        createSettingsControls();

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
                        // the size follows from the layout, so only the suggested position
                        // is taken, and only from a window the user can see moving
                        refreshSettingsFonts();
                        // the list checkboxes are drawn at a size for the dpi
                        applySettingsAppearance();
                        layoutSettings();

                        if (settings.isVisible())
                        {
                          ::SetWindowPos(settings.hwnd.get(), nullptr, settings.suggested.x,
                                         settings.suggested.y, 0, 0,
                                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
                        }

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
                   settings.isVisible() ? cancelSettings() : showSettings();

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
                   if (sah->settingsFolder.has_value())
                   {
                     auto loadPath{*sah->settingsFolder};

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

                 case Menu::Identifier::RestoreLastSession:
                 {
                   toggleRestoreLastSession();

                   return 0;
                 }
               }

               ::DefWindowProcW(msg.hwnd, msg.msg, msg.wparam, msg.lparam);

               return 0;
             });

  // the system menu shows the choice in effect, which the plugin may have changed since
  message.on(WM_INITMENUPOPUP,
             [this](Message msg)
             {
               if (HIWORD(msg.lparam)) showRestoreLastSession(reinterpret_cast<::HMENU>(msg.wparam));

               ::DefWindowProcW(msg.hwnd, msg.msg, msg.wparam, msg.lparam);

               return 0;
             });

  // audio is applied as it is chosen (applyShownAudio), MIDI and the rest only by OK;
  // Cancel goes back to the saved setup
  settings.message.on(
      WM_COMMAND,
      [this](Message msg)
      {
        if (!sah) return 0;

        const auto id{LOWORD(msg.wparam)};
        const auto code{HIWORD(msg.wparam)};

        switch (id)
        {
          // also what Enter and Esc send, through IsDialogMessage
          case IDOK:
            okSettings();
            break;

          case IDCANCEL:
            cancelSettings();
            break;

          case Settings::Identifier::ResetToDefault:
            if (code == BN_CLICKED) resetSettingsToDefault();
            break;

          case Settings::Identifier::AudioApi:
            if (code == CBN_SELCHANGE) selectApiInSettings();
            break;

          case Settings::Identifier::AudioOutput:
          case Settings::Identifier::AudioInput:
            if (code == CBN_SELCHANGE)
            {
              // the same channels on the other device, where it has them
              refreshChannelChoices(shownChannels(false).first, shownChannels(true).first);
              refreshSampleRates(shownSampleRate());
              refreshBufferSizes(shownBufferSize());
              refreshInputNote();
              refreshDriverPanelButton();
              applyShownAudio();
            }
            break;

          case Settings::Identifier::DriverControlPanel:
            if (code == BN_CLICKED) showDriverControlPanel();
            break;

          case Settings::Identifier::AudioSamplerate:
            if (code == CBN_SELCHANGE)
            {
              refreshBufferSizeTitles();
              applyShownAudio();
            }
            break;

          case Settings::Identifier::AudioBuffersize:
          case Settings::Identifier::AudioOutputChannels:
          case Settings::Identifier::AudioInputChannels:
            if (code == CBN_SELCHANGE) applyShownAudio();
            break;

          // a checkbox's own text cannot take the window's colours, so its
          // label is a separate control that ticks it
          case Settings::Identifier::VirtualInputLabel:
          case Settings::Identifier::VirtualOutputLabel:
            if (code == STN_CLICKED)
            {
              auto &check{id == Settings::Identifier::VirtualInputLabel ? settings.virtualIn
                                                                        : settings.virtualOut};
              const auto checked{check.message.send(check.hwnd.get(), BM_GETCHECK, 0, 0)};
              check.message.send(check.hwnd.get(), BM_SETCHECK,
                                 checked == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED, 0);
            }
            break;

          // tried at once, through onScreenAppearanceChanged; kept by OK
          case Settings::Identifier::Appearance:
            if (code == CBN_SELCHANGE)
            {
              if (auto index{settings.appearance.selection(settings.appearancesShown.size())}; index)
              {
                sah->setScreenAppearance(settings.appearancesShown[*index], false);
              }
            }
            break;
        }

        return 0;
      });

  // a click on a port's name ticks it, as on its box
  settings.message.on(
      WM_NOTIFY,
      [this](Message msg)
      {
        auto header{reinterpret_cast<::LPNMHDR>(msg.lparam)};

        if (header->code == NM_CLICK && (header->idFrom == Settings::Identifier::MidiInputs ||
                                         header->idFrom == Settings::Identifier::MidiOutputs))
        {
          auto &list{header->idFrom == Settings::Identifier::MidiInputs ? settings.midiIn
                                                                        : settings.midiOut};

          ::LVHITTESTINFO hit{};
          hit.pt = reinterpret_cast<::LPNMITEMACTIVATE>(msg.lparam)->ptAction;

          if (ListView_HitTest(header->hwndFrom, &hit) >= 0 && (hit.flags & LVHT_ONITEMLABEL) &&
              static_cast<size_t>(hit.iItem) < list.rows.size())
          {
            const auto row{static_cast<size_t>(hit.iItem)};
            list.setOn(row, !list.isOn(row));
          }
        }

        return 0;
      });

  settings.message.on(DM_GETDEFID,
                      [](Message) { return static_cast<::LRESULT>(MAKELRESULT(IDOK, DC_HASDEFID)); });

  settings.message.on(
      WM_CTLCOLORSTATIC,
      [this](Message msg)
      {
        auto dc{reinterpret_cast<::HDC>(msg.wparam)};
        auto control{reinterpret_cast<::HWND>(msg.lparam)};
        const auto &palette{settings.palette};

        if (control == settings.midiIn.empty.hwnd.get() || control == settings.midiOut.empty.hwnd.get())
        {
          ::SetTextColor(dc, palette.secondary);
          ::SetBkColor(dc, palette.field);
          return reinterpret_cast<::LRESULT>(settings.fieldBrush.get());
        }

        auto color{palette.text};
        if (control == settings.noRateNote.hwnd.get()) color = palette.critical;
        if (control == settings.inputNote.hwnd.get())
          color = settings.inputNoteCaution ? palette.caution : palette.secondary;
        if (control == settings.latencyNote.hwnd.get()) color = palette.secondary;

        ::SetTextColor(dc, color);
        ::SetBkColor(dc, palette.background);
        return reinterpret_cast<::LRESULT>(settings.backgroundBrush.get());
      });

  settings.message.on(WM_CTLCOLORBTN,
                      [this](Message msg)
                      {
                        ::SetBkColor(reinterpret_cast<::HDC>(msg.wparam), settings.palette.background);
                        return reinterpret_cast<::LRESULT>(settings.backgroundBrush.get());
                      });

  // the drop-down lists of the combo boxes
  settings.message.on(WM_CTLCOLORLISTBOX,
                      [this](Message msg)
                      {
                        auto dc{reinterpret_cast<::HDC>(msg.wparam)};
                        ::SetTextColor(dc, settings.palette.text);
                        ::SetBkColor(dc, settings.palette.field);
                        return reinterpret_cast<::LRESULT>(settings.fieldBrush.get());
                      });

  settings.message.on(WM_ERASEBKGND,
                      [this](Message msg)
                      {
                        ::RECT rect{};
                        ::GetClientRect(msg.hwnd, &rect);
                        ::FillRect(reinterpret_cast<::HDC>(msg.wparam), &rect,
                                   settings.backgroundBrush.get());
                        return 1;
                      });

  settings.message.on(WM_PAINT,
                      [this](Message msg)
                      {
                        ::PAINTSTRUCT ps;
                        ::HDC hdc{::BeginPaint(msg.hwnd, &ps)};

                        wil::unique_hbrush rule{::CreateSolidBrush(settings.palette.rule)};
                        for (const auto &rect : settings.rules) ::FillRect(hdc, &rect, rule.get());

                        ::EndPaint(msg.hwnd, &ps);

                        return 0;
                      });

  // "auto" follows the system as it changes
  settings.message.on(
      WM_SETTINGCHANGE,
      [this](Message msg)
      {
        if (sah && sah->screenAppearance == CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO && msg.lparam &&
            ::lstrcmpW(reinterpret_cast<::LPCWSTR>(msg.lparam), L"ImmersiveColorSet") == 0)
        {
          applySettingsAppearance();
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

                 // an inactive plugin has nothing to restart
                 if (sah->restartRequested.exchange(false) && sah->isActive)
                 {
                   // Reactivate in place rather than bouncing the whole audio
                   // engine. activatePlugin parks the callback, reactivates, and
                   // only then lets processing resume - and leaves it stopped if
                   // the plugin refuses to reactivate.
                   sah->activatePlugin(sah->currentSampleRate, 1, sah->currentBufferSize * 2);
                 }

                 // a stream that stopped on its own, devices that came or went
                 sah->pollAudioEvents();

                 // an error is shown once while it stands; a stream that has run again
                 // for a while ends it, so that the next failure, with the same words, is
                 // shown again. a moment is not enough: a device just plugged back in
                 // often stops the stream reopened on it once more
                 const auto now{std::chrono::steady_clock::now()};
                 const bool wasRunning{audioRunningSince.has_value()};
                 if (!sah->isAudioRunning())
                 {
                   audioRunningSince.reset();
                 }
                 else if (!audioRunningSince)
                 {
                   audioRunningSince = now;
                 }
                 else if (now - *audioRunningSince > std::chrono::seconds(3))
                 {
                   std::lock_guard<std::mutex> guard(pendingErrorLock);
                   lastShownError.clear();
                 }

                 // a stream that stopped or opened again while the window is up: the
                 // latency beside the buffer size is that stream's, or none
                 if (settings.isVisible() && wasRunning != audioRunningSince.has_value())
                   layoutSettings();

                 // an asio driver asked for a reset, and rtaudio has closed the stream for
                 // it: the same setup again, which reads back what the driver now grants.
                 // not from inside the driver's own control panel call
                 if (!settings.inDriverPanel && sah->takeDriverReset())
                 {
                   startAudio();
                   if (settings.isVisible())
                   {
                     refreshBufferSizes(sah->currentBufferSize);
                     layoutSettings();
                   }
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
               sah->onAudioDevicesChanged = nullptr;
               sah->onShowSettingsScreen = nullptr;
               sah->onScreenAppearanceChanged = nullptr;
               settingsWindowOwner = nullptr;

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

  // the title bar's close button, which is Cancel
  settings.message.on(WM_CLOSE,
                      [this](Message msg)
                      {
                        cancelSettings();

                        return 0;
                      });

  // the settings window was closed: tell the plugin, now that it is gone and from the
  // message loop rather than from whichever handler hid it
  message.on(WM_SETTINGS_CLOSED,
             [this](Message msg)
             {
               if (sah)
                 sah->settingsScreenClosed(msg.wparam ? CLAP_WRAPPER_STANDALONE_CONFIGURATION_APPLIED
                                                      : CLAP_WRAPPER_STANDALONE_CONFIGURATION_CANCELLED);

               return 0;
             });

  create(OUTPUT_NAME);

  settings.create("Audio/MIDI Settings");
  settingsWindowOwner = this;

  // a plugin may set the appearance at any time, on an open window too
  sah->onScreenAppearanceChanged = [this]()
  {
    if (!settings.hwnd) return;
    applySettingsAppearance();
    if (settings.isVisible())
    {
      refreshAppearanceRow();
      layoutSettings();
    }
  };

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

  // Never put the message box up from here. The host calls this from wherever the
  // failure happened - including from inside the handler that asked for the device
  // change. A modal box runs a nested message
  // loop, so showing one there re-enters the handler that is still on the stack.
  // Queue the text and let the main loop show it once it is idle.
  // the settings window, while it is up, follows devices and ports coming and going
  sah->onAudioDevicesChanged = [this]() { devicesChanged(); };

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
               // one box at a time: what arrives while it is up is the same failure
               if (showingAudioError)
               {
                 // dropped, not kept: a pending text left behind would match, and so hide,
                 // every later error in the same words
                 std::lock_guard<std::mutex> guard(pendingErrorLock);
                 pendingError.clear();
                 return 0;
               }

               std::string text;
               {
                 std::lock_guard<std::mutex> guard(pendingErrorLock);
                 text = pendingError;
                 pendingError.clear();
                 lastShownError = text;
               }

               if (!text.empty())
               {
                 showingAudioError = true;
                 message.error("Unable to configure audio: {}", text);
                 showingAudioError = false;
               }

               return 0;
             });

  startAudio();

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

void Plugin::createSettingsControls()
{
  using Id = Settings::Identifier;
  auto parent{settings.hwnd.get()};

  // the label column is right-aligned, next to what it names
  auto label = [parent](Control &control, const char *text)
  { control.create(WC_STATICW, text, SS_RIGHT | SS_NOPREFIX, Id::Static, parent); };
  auto note = [parent](Control &control, const char *text)
  { control.create(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, Id::Static, parent); };

  // in tab order
  settings.heading.create(WC_STATICW, "", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, Id::Static, parent);

  label(settings.apiLabel, "Audio API:");
  settings.api.create("Audio API", Id::AudioApi, parent);
  label(settings.outputLabel, "Output:");
  settings.output.create("Output", Id::AudioOutput, parent);
  label(settings.outputChannelsLabel, "Output Channels:");
  settings.outputChannels.create("Output Channels", Id::AudioOutputChannels, parent);
  label(settings.inputLabel, "Input:");
  settings.input.create("Input", Id::AudioInput, parent);
  label(settings.inputChannelsLabel, "Input Channels:");
  settings.inputChannels.create("Input Channels", Id::AudioInputChannels, parent);
  note(settings.inputNote, "");
  label(settings.sampleRateLabel, "Sample Rate:");
  settings.sampleRate.create("Sample Rate", Id::AudioSamplerate, parent);
  label(settings.bufferSizeLabel, "Buffer Size:");
  settings.bufferSize.create("Buffer Size", Id::AudioBuffersize, parent);
  settings.resetToDefault.create(WC_BUTTONW, "Reset to System Default", BS_PUSHBUTTON | WS_TABSTOP,
                                 Id::ResetToDefault, parent);
  settings.driverPanel.create(WC_BUTTONW, "ASIO Control Panel...", BS_PUSHBUTTON | WS_TABSTOP,
                              Id::DriverControlPanel, parent);
  note(settings.noRateNote, "These devices have no sample rate in common.");
  settings.latencyNote.create(WC_STATICW, "", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, Id::Static,
                              parent);

  label(settings.midiInLabel, "MIDI Inputs:");
  settings.midiIn.create(Id::MidiInputs, parent);
  settings.virtualIn.create(WC_BUTTONW, "", BS_AUTOCHECKBOX | WS_TABSTOP, Id::VirtualInput, parent);
  settings.virtualInLabel.create(WC_STATICW, "", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_ENDELLIPSIS,
                                 Id::VirtualInputLabel, parent);
  label(settings.midiOutLabel, "MIDI Outputs:");
  settings.midiOut.create(Id::MidiOutputs, parent);
  settings.virtualOut.create(WC_BUTTONW, "", BS_AUTOCHECKBOX | WS_TABSTOP, Id::VirtualOutput, parent);
  settings.virtualOutLabel.create(WC_STATICW, "", SS_LEFT | SS_NOPREFIX | SS_NOTIFY | SS_ENDELLIPSIS,
                                  Id::VirtualOutputLabel, parent);

  label(settings.appearanceLabel, "Appearance:");
  settings.appearance.create("Appearance", Id::Appearance, parent);

  // OK first: the order windows puts them in
  settings.ok.create(WC_BUTTONW, "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, parent);
  settings.cancel.create(WC_BUTTONW, "Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, parent);

  refreshSettingsFonts();
  applySettingsAppearance();
}

void Plugin::refreshSettingsFonts()
{
  settings.font.reset(getMessageFont(settings.dpi));
  settings.headingFont.reset(getMessageFont(settings.dpi, 115, FW_SEMIBOLD));

  for (auto *control : {&settings.apiLabel,
                        &settings.outputLabel,
                        &settings.outputChannelsLabel,
                        &settings.inputLabel,
                        &settings.inputChannelsLabel,
                        &settings.sampleRateLabel,
                        &settings.bufferSizeLabel,
                        &settings.inputNote,
                        &settings.noRateNote,
                        &settings.latencyNote,
                        &settings.midiInLabel,
                        &settings.midiOutLabel,
                        &settings.virtualInLabel,
                        &settings.virtualOutLabel,
                        &settings.appearanceLabel,
                        &settings.midiIn.empty,
                        &settings.midiOut.empty,
                        &settings.resetToDefault,
                        &settings.driverPanel,
                        &settings.ok,
                        &settings.cancel,
                        &settings.virtualIn,
                        &settings.virtualOut})
  {
    control->setFont(settings.font.get());
  }

  for (auto *control :
       {&settings.api, &settings.output, &settings.outputChannels, &settings.input,
        &settings.inputChannels, &settings.sampleRate, &settings.bufferSize, &settings.appearance})
  {
    control->setFont(settings.font.get());
  }

  settings.midiIn.setFont(settings.font.get());
  settings.midiOut.setFont(settings.font.get());

  settings.heading.setFont(settings.headingFont.get());
}

void Plugin::populateSettings()
{
  auto &saved{sah->settings};

  settings.apiWhenOpened = sah->audioApiName;
  settings.apiChanged = false;
  settings.audioOpenedEarly = false;
  settings.appearanceWhenOpened = sah->screenAppearance;

  settings.heading.setText("Audio/MIDI Setup for " + pluginDisplayName(plugin.plugin));

  refreshApis();
  // the input is the default's even with no input open, and showing that device would have
  // OK open it
  refreshDevices(sah->audioOutputDevice, sah->audioInputDevice, sah->audioInputUsed);
  // a mono main input takes one channel, anything else a pair
  settings.inputChannelsLabel.setText(sah->mainBusChannels(true) == 1 ? "Input Channel:"
                                                                      : "Input Channels:");
  refreshChannelChoices(sah->deviceOutputFirstChannel, sah->deviceInputFirstChannel);
  refreshSampleRates(sah->currentSampleRate);
  refreshBufferSizes(sah->currentBufferSize);
  refreshInputNote();
  refreshDriverPanelButton();

  // midi: a direction the plugin has no note port for gets no section at all
  std::vector<std::string> onIds, absentNames;
  settings.midiInAllOn = saved.midiBindAllPorts;
  settings.midiIn.rows.clear();
  settings.midiOut.rows.clear();
  sah->savedMidiSelection(true, onIds, absentNames);
  settings.midiIn.fill(sah->getMidiInputPorts(), onIds, absentNames, settings.midiInAllOn);
  sah->savedMidiSelection(false, onIds, absentNames);
  settings.midiOut.fill(sah->getMidiOutputPorts(), onIds, absentNames, false);
  settings.midiIn.empty.setText(" No MIDI inputs found");
  settings.midiOut.empty.setText(" No MIDI outputs found");

  // offered where the midi api has virtual ports, which winmm does not
  const bool virtualPorts{sah->supportsVirtualMidiPorts()};
  settings.virtualInWasOn = saved.midiVirtualInput;
  settings.virtualOutWasOn = saved.midiVirtualOutput;
  settings.virtualIn.message.send(settings.virtualIn.hwnd.get(), BM_SETCHECK,
                                  settings.virtualInWasOn ? BST_CHECKED : BST_UNCHECKED, 0);
  settings.virtualOut.message.send(settings.virtualOut.hwnd.get(), BM_SETCHECK,
                                   settings.virtualOutWasOn ? BST_CHECKED : BST_UNCHECKED, 0);
  settings.virtualInLabel.setText("Virtual input \xE2\x80\x9C" + sah->virtualMidiPortName(true) +
                                  "\xE2\x80\x9D");
  settings.virtualOutLabel.setText("Virtual output \xE2\x80\x9C" + sah->virtualMidiPortName(false) +
                                   "\xE2\x80\x9D");

  for (auto *control : {&settings.midiInLabel, &static_cast<Control &>(settings.midiIn)})
    control->setVisible(takesMidi());
  for (auto *control : {&settings.virtualIn, &settings.virtualInLabel})
    control->setVisible(takesMidi() && virtualPorts);
  for (auto *control : {&settings.midiOutLabel, &static_cast<Control &>(settings.midiOut)})
    control->setVisible(sendsMidi());
  for (auto *control : {&settings.virtualOut, &settings.virtualOutLabel})
    control->setVisible(sendsMidi() && virtualPorts);

  // appearance
  settings.appearance.reset();
  settings.appearancesShown.clear();
  const auto appearances{supportedScreenAppearances()};
  const std::pair<uint32_t, const char *> appearanceItems[] = {
      {CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO, "Auto"},
      {CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT, "Light"},
      {CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK, "Dark"}};
  for (const auto &[appearance, title] : appearanceItems)
  {
    if (!(appearances & (1u << appearance))) continue;
    settings.appearance.add(title);
    settings.appearancesShown.push_back(appearance);
  }
  refreshAppearanceRow();
  applySettingsAppearance();
}

// the driver's control panel, which asio has: offered while the window shows an asio
// driver, and usable once there is a driver to show it for
void Plugin::refreshDriverPanelButton()
{
  settings.driverPanel.setVisible(sah->hasDriverControlPanel());
  ::EnableWindow(settings.driverPanel.hwnd.get(), shownOutput() ? TRUE : FALSE);
}

void Plugin::refreshApis()
{
  settings.api.reset();
  settings.apisShown = sah->getAudioApis();

  for (size_t i = 0; i < settings.apisShown.size(); ++i)
  {
    settings.api.add(settings.apisShown[i].name);
    if (settings.apisShown[i].id == sah->audioApiName) settings.api.set(static_cast<int>(i));
  }

  // a choice of one is no choice
  settings.apiLabel.setVisible(settings.apisShown.size() > 1);
  settings.api.setVisible(settings.apisShown.size() > 1);
}

void Plugin::refreshDevices(const std::string &outputId, const std::string &inputId, bool useInput)
{
  settings.outDevices = sah->getOutputAudioDevices();
  settings.inDevices = sah->getInputAudioDevices();

  // two devices of one name get " (2)" and so on
  auto titles = [](const std::vector<AudioDevice> &devices)
  {
    std::vector<const AudioDevice *> pointers;
    for (const auto &device : devices) pointers.push_back(&device);
    return deviceMenuTitles(pointers);
  };

  settings.output.reset();
  int selected{settings.outDevices.empty() ? -1 : 0};
  const auto outTitles{titles(settings.outDevices)};
  for (size_t i = 0; i < settings.outDevices.size(); ++i)
  {
    settings.output.add(outTitles[i]);
    if (settings.outDevices[i].id == outputId) selected = static_cast<int>(i);
  }
  settings.output.set(selected);

  settings.input.reset();
  settings.input.add("No Input");
  selected = 0;
  const auto inTitles{titles(settings.inDevices)};
  for (size_t i = 0; i < settings.inDevices.size(); ++i)
  {
    settings.input.add(inTitles[i]);
    if (useInput && settings.inDevices[i].id == inputId) selected = static_cast<int>(i + 1);
  }
  settings.input.set(selected);
}

void Plugin::devicesChanged()
{
  if (!settings.isVisible()) return;

  const auto out{shownOutput()};
  const auto in{shownInput()};
  const auto rate{shownSampleRate()};
  const auto frames{shownBufferSize()};
  const auto outFirst{shownChannels(false).first};
  const auto inFirst{shownChannels(true).first};

  refreshDevices(out ? settings.outDevices[*out].id : std::string{},
                 in ? settings.inDevices[*in].id : std::string{}, in.has_value());
  refreshChannelChoices(outFirst, inFirst);
  refreshSampleRates(rate);
  refreshBufferSizes(frames);
  refreshInputNote();
  refreshDriverPanelButton();

  // rows already there keep what they show; a port plugged in now is ticked as the
  // settings would open it, and a saved one not plugged in stays listed
  std::vector<std::string> onIds, absentNames;
  sah->savedMidiSelection(true, onIds, absentNames);
  settings.midiIn.fill(sah->getMidiInputPorts(), onIds, absentNames, settings.midiInAllOn);
  sah->savedMidiSelection(false, onIds, absentNames);
  settings.midiOut.fill(sah->getMidiOutputPorts(), onIds, absentNames, false);

  layoutSettings();
}

std::optional<size_t> Plugin::shownOutput()
{
  return settings.output.selection(settings.outDevices.size());
}

std::optional<size_t> Plugin::shownInput()
{
  // "No Input" is the first entry, so the devices are one further down
  auto index{settings.input.selection(settings.inDevices.size() + 1)};
  if (!index || *index == 0) return std::nullopt;
  return *index - 1;
}

int32_t Plugin::shownSampleRate()
{
  auto index{settings.sampleRate.selection(settings.sampleRatesShown.size())};
  return index ? settings.sampleRatesShown[*index] : 0;
}

uint32_t Plugin::shownBufferSize()
{
  auto index{settings.bufferSize.selection(settings.bufferSizesShown.size())};
  return index ? settings.bufferSizesShown[*index] : 0;
}

AudioChannelChoice Plugin::shownChannels(bool input)
{
  const auto &choices{input ? settings.inChoices : settings.outChoices};
  auto index{(input ? settings.inputChannels : settings.outputChannels).selection(choices.size())};
  return index ? choices[*index] : AudioChannelChoice{};
}

void Plugin::refreshChannelChoices(uint32_t outputFirst, uint32_t inputFirst)
{
  auto fill = [](ComboBox &menu, std::vector<AudioChannelChoice> &choices,
                 std::vector<AudioChannelChoice> from, uint32_t first)
  {
    choices = std::move(from);
    menu.reset();
    int selected{choices.empty() ? -1 : 0};
    for (size_t i = 0; i < choices.size(); ++i)
    {
      menu.add(choices[i].title);
      if (choices[i].first == first) selected = static_cast<int>(i);
    }
    menu.set(selected);
    // a choice of one is shown, and is no choice
    ::EnableWindow(menu.hwnd.get(), choices.size() > 1 ? TRUE : FALSE);
  };

  const auto out{shownOutput()};
  const auto in{shownInput()};
  fill(settings.outputChannels, settings.outChoices,
       out ? sah->getChannelChoices(settings.outDevices[*out].id, false)
           : std::vector<AudioChannelChoice>{},
       outputFirst);
  fill(settings.inputChannels, settings.inChoices,
       in ? sah->getChannelChoices(settings.inDevices[*in].id, true) : std::vector<AudioChannelChoice>{},
       inputFirst);

  const bool inputRow{in.has_value() && sah->numAudioInputs > 0};
  settings.inputChannelsLabel.setVisible(inputRow);
  settings.inputChannels.setVisible(inputRow);
}

// the rates the selected devices share: one stream runs both at one rate
void Plugin::refreshSampleRates(int32_t preferred)
{
  settings.sampleRate.reset();
  settings.sampleRatesShown.clear();

  const auto out{shownOutput()};
  if (!out)
  {
    // no output device at all: nothing to run a stream on
    ::EnableWindow(settings.ok.hwnd.get(), FALSE);
    settings.noRateNote.setVisible(false);
    return;
  }

  std::vector<unsigned int> rates{settings.outDevices[*out].sampleRates};
  std::sort(rates.begin(), rates.end());

  if (const auto in{shownInput()}; in)
  {
    auto inRates{settings.inDevices[*in].sampleRates};
    std::sort(inRates.begin(), inRates.end());

    std::vector<unsigned int> both;
    std::set_intersection(rates.begin(), rates.end(), inRates.begin(), inRates.end(),
                          std::back_inserter(both));
    rates = both;
  }
  rates.erase(std::unique(rates.begin(), rates.end()), rates.end());

  // the preferred rate where the devices have it, else 44100, else 48000: the first entry
  // would be 4000 Hz on wasapi
  const auto shown{preferredSampleRate(rates, preferred > 0 ? static_cast<uint32_t>(preferred) : 0)};
  int selected{-1};
  for (size_t i = 0; i < rates.size(); ++i)
  {
    settings.sampleRatesShown.push_back(static_cast<int32_t>(rates[i]));
    settings.sampleRate.add(std::to_string(rates[i]));
    if (rates[i] == shown) selected = static_cast<int>(i);
  }
  settings.sampleRate.set(selected);

  // no rate in common, nothing to open the two devices at
  const bool canOpen{!rates.empty()};
  ::EnableWindow(settings.ok.hwnd.get(), canOpen ? TRUE : FALSE);
  settings.noRateNote.setVisible(!canOpen);
}

// the sizes both selected devices accept, from the standalone's list; selects the
// preferred size, or the largest below it, or the smallest there is
void Plugin::refreshBufferSizes(uint32_t preferred)
{
  const auto out{shownOutput()};
  const auto in{shownInput()};
  auto sizes{sah->getBufferSizesFor(out ? settings.outDevices[*out].id : std::string{},
                                    in ? settings.inDevices[*in].id : std::string{})};

  // the stream may run at a size the list does not have, which the device chose: a
  // driver fixed at 1024 shows 1024, not the first entry
  const auto running{sah->currentBufferSize};
  if (running > 0 && std::find(sizes.begin(), sizes.end(), running) == sizes.end())
  {
    sizes.insert(std::upper_bound(sizes.begin(), sizes.end(), running), running);
  }

  if (preferred == 0) preferred = StandaloneSettings::defaultBufferSize;

  settings.bufferSizesShown = sizes;

  int selected{sizes.empty() ? -1 : 0};
  for (size_t i = 0; i < sizes.size(); ++i)
  {
    if (sizes[i] <= preferred) selected = static_cast<int>(i);
  }

  refreshBufferSizeTitles();
  settings.bufferSize.set(selected);
}

// each size with how long it lasts at the selected rate, "256 (5.8 ms)"
void Plugin::refreshBufferSizeTitles()
{
  const auto selected{settings.bufferSize.get()};
  const auto rate{shownSampleRate()};

  settings.bufferSize.reset();
  for (auto frames : settings.bufferSizesShown)
  {
    settings.bufferSize.add(
        rate > 0 ? fmt::format("{} ({:.1f} ms)", frames,
                               1000.0 * static_cast<double>(frames) / static_cast<double>(rate))
                 : std::to_string(frames));
  }
  settings.bufferSize.set(static_cast<int>(selected));
}

// the latencies the running stream's devices report, beside the buffer size and not added
// to it: they are the device's and the driver's, which no buffer size changes. shown only
// while the window shows what runs - another device, rate or size would have others - and
// only what the backend could tell
void Plugin::refreshLatencyNote()
{
  const auto out{shownOutput()};
  const auto in{shownInput()};
  const auto &stream{sah->currentStream};
  const auto outChannels{shownChannels(false)};
  const auto inChannels{shownChannels(true)};

  const bool asRunning{sah->isAudioRunning() && !settings.apiChanged && stream.sampleRate > 0 &&
                       shownSampleRate() == static_cast<int32_t>(stream.sampleRate) &&
                       shownBufferSize() == stream.bufferFrames && out &&
                       settings.outDevices[*out].id == sah->audioOutputDevice &&
                       outChannels.first == sah->deviceOutputFirstChannel &&
                       (in ? stream.inputChannels > 0 &&
                                 settings.inDevices[*in].id == sah->audioInputDevice &&
                                 inChannels.first == sah->deviceInputFirstChannel
                           : stream.inputChannels == 0)};

  std::string text;
  if (asRunning)
  {
    auto ms = [&stream](uint32_t frames)
    { return fmt::format("{:.1f} ms", 1000.0 * frames / static_cast<double>(stream.sampleRate)); };
    if (in && stream.inputLatency > 0) text = "in " + ms(stream.inputLatency);
    if (stream.outputLatency > 0) text += (text.empty() ? "out " : ", out ") + ms(stream.outputLatency);
  }

  settings.latencyNote.setText(text.empty() ? std::string{} : "Latency: " + text);
  settings.latencyNote.setVisible(!text.empty());
}

// under the input menu, while it matters: that a bluetooth microphone takes its whole
// device down to telephone quality, or that the default input is a microphone and was not
// opened
void Plugin::refreshInputNote()
{
  std::string text;
  settings.inputNoteCaution = false;

  const auto in{shownInput()};
  if (in && settings.inDevices[*in].isBluetooth)
  {
    text =
        "A Bluetooth microphone switches its device to telephone quality, for every application, "
        "while it is open.";
    settings.inputNoteCaution = true;
  }
  else if (!in && sah->numAudioInputs > 0)
  {
    auto [input, output, rate] = sah->getDefaultAudioInOutSampleRate();
    const auto *device{sah->findDevice(input)};
    if (device && device->isMicrophone)
      text = "The default input is a microphone, which is opened only when chosen here.";
  }

  settings.inputNote.setText(text);
  settings.inputNote.setVisible(!text.empty());
}

// the window's own appearance menu, offered while the plugin has not set the appearance
// itself (StandaloneHost::screenAppearanceFromPlugin)
void Plugin::refreshAppearanceRow()
{
  for (size_t i = 0; i < settings.appearancesShown.size(); ++i)
  {
    if (settings.appearancesShown[i] == sah->screenAppearance)
      settings.appearance.set(static_cast<int>(i));
  }

  const bool shown{settings.appearancesShown.size() > 1 && !sah->screenAppearanceFromPlugin};
  settings.appearanceLabel.setVisible(shown);
  settings.appearance.setVisible(shown);
}

void Plugin::applySettingsAppearance()
{
  const bool dark{isDark(sah ? sah->screenAppearance : CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO)};

  settings.palette = paletteFor(dark);
  settings.backgroundBrush.reset(::CreateSolidBrush(settings.palette.background));
  settings.fieldBrush.reset(::CreateSolidBrush(settings.palette.field));

  // the title bar; 20 from Windows 10 20H1, 19 before it
  ::BOOL useDark{dark ? TRUE : FALSE};
  if (FAILED(::DwmSetWindowAttribute(settings.hwnd.get(), 20, &useDark, sizeof(useDark))))
  {
    ::DwmSetWindowAttribute(settings.hwnd.get(), 19, &useDark, sizeof(useDark));
  }

  // a title bar already on screen keeps its colours until its frame is drawn again, which
  // windows 10 does only on a change of activation
  if (settings.isVisible())
  {
    ::SetWindowPos(settings.hwnd.get(), nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (::GetActiveWindow() == settings.hwnd.get())
    {
      ::DefWindowProcW(settings.hwnd.get(), WM_NCACTIVATE, FALSE, 0);
      ::DefWindowProcW(settings.hwnd.get(), WM_NCACTIVATE, TRUE, 0);
    }
  }

  for (auto *combo :
       {&settings.api, &settings.output, &settings.outputChannels, &settings.input,
        &settings.inputChannels, &settings.sampleRate, &settings.bufferSize, &settings.appearance})
  {
    ::SetWindowTheme(combo->hwnd.get(), dark ? L"DarkMode_CFD" : nullptr, nullptr);
  }

  for (auto *button : {&settings.resetToDefault, &settings.driverPanel, &settings.ok, &settings.cancel,
                       &settings.virtualIn, &settings.virtualOut})
  {
    ::SetWindowTheme(button->hwnd.get(), dark ? L"DarkMode_Explorer" : nullptr, nullptr);
  }

  settings.midiIn.setTheme(dark, settings.palette.field, settings.palette.text,
                           settings.palette.secondary);
  settings.midiOut.setTheme(dark, settings.palette.field, settings.palette.text,
                            settings.palette.secondary);

  ::RedrawWindow(settings.hwnd.get(), nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void Plugin::layoutSettings()
{
  refreshLatencyNote();

  const auto dipScale{settings.scale};
  auto px = [dipScale](double dip) { return static_cast<int>(std::lround(dip * dipScale)); };

  const auto font{settings.font.get()};
  const auto edge{::GetSystemMetricsForDpi(SM_CYEDGE, settings.dpi)};
  const auto lineHeight{static_cast<int>(textExtent(font, settings.outputLabel.hwnd.get()).cy)};

  // the label column is as wide as its widest visible label
  int labelWidth{0};
  for (auto *label :
       {&settings.apiLabel, &settings.outputLabel, &settings.outputChannelsLabel, &settings.inputLabel,
        &settings.inputChannelsLabel, &settings.sampleRateLabel, &settings.bufferSizeLabel,
        &settings.midiInLabel, &settings.midiOutLabel, &settings.appearanceLabel})
  {
    if (label->visible)
      labelWidth = std::max(labelWidth, static_cast<int>(textExtent(font, label->hwnd.get()).cx));
  }

  const int left{px(windowMargin)};
  const int controlX{left + labelWidth + px(columnSpacing)};
  const int width{px(controlWidth)};
  const int right{controlX + width};
  int y{px(headingTop)};

  settings.rules.clear();
  auto rule = [&](int padding)
  {
    y += padding;
    settings.rules.push_back({left, y, right, y + std::max(1, px(1))});
    y += std::max(1, px(1)) + padding;
  };

  // a label is centred on the line it names
  auto placeLabel = [&](Control &label, int centreY)
  { label.setPosition(left, centreY - lineHeight / 2, labelWidth, lineHeight); };

  // a combo box sizes its own closed height from its font; the height given is the drop-down's
  auto comboRow = [&](Control &label, ComboBox &combo, int comboWidth)
  {
    if (!combo.visible) return;
    ::SetWindowPos(combo.hwnd.get(), nullptr, controlX, y, comboWidth, px(dropDownHeight),
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::RECT rect{};
    ::GetWindowRect(combo.hwnd.get(), &rect);
    const int height{rect.bottom - rect.top};
    placeLabel(label, y + height / 2);
    y += height + px(rowSpacing);
  };

  auto noteRow = [&](Control &note)
  {
    if (!note.visible) return;
    const int height{wrappedHeight(font, note.hwnd.get(), width)};
    note.setPosition(controlX, y, width, height);
    y += height + px(rowSpacing);
  };

  auto buttonWidth = [&](Control &button)
  {
    return std::max(px(buttonMinWidth),
                    static_cast<int>(textExtent(font, button.hwnd.get()).cx) + 2 * px(buttonPadding));
  };

  // a list has no line of its own: its label is centred on its first row
  auto listRow = [&](Control &label, PortList &list)
  {
    if (!list.visible)
    {
      list.empty.setVisible(false);
      return;
    }

    const bool isEmpty{list.rows.empty()};
    const int rowHeight{isEmpty ? lineHeight + px(4) : static_cast<int>(list.rowHeight())};

    // a list that scrolls shows half a row more, so that it plainly does
    const bool scrolls{list.rows.size() > visiblePortRows};
    const double shownRows{
        static_cast<double>(std::clamp(list.rows.size(), size_t{1}, visiblePortRows)) +
        (scrolls ? 0.5 : 0.0)};
    const int height{static_cast<int>(std::lround(shownRows * rowHeight)) + 4 * edge};

    ::ShowWindow(list.hwnd.get(), isEmpty ? SW_HIDE : SW_SHOW);
    list.empty.setVisible(isEmpty);

    if (isEmpty)
    {
      list.empty.setPosition(controlX, y, width, rowHeight + 4 * edge);
      placeLabel(label, y + 2 * edge + rowHeight / 2);
      y += rowHeight + 4 * edge + px(rowSpacing);
      return;
    }

    list.setPosition(controlX, y, width, height);
    ::RECT client{};
    ::GetClientRect(list.hwnd.get(), &client);
    ListView_SetColumnWidth(list.hwnd.get(), 0, client.right - client.left);

    placeLabel(label, y + 2 * edge + rowHeight / 2);
    y += height + px(rowSpacing);
  };

  auto checkRow = [&](Control &check, Control &label)
  {
    if (!check.visible) return;
    const int box{::GetSystemMetricsForDpi(SM_CXMENUCHECK, settings.dpi)};
    const int height{std::max(box, lineHeight)};
    check.setPosition(controlX, y + (height - box) / 2, box, box);
    label.setPosition(controlX + box + px(4), y + (height - lineHeight) / 2, width - box - px(4),
                      lineHeight);
    y += height + px(rowSpacing);
  };

  // heading
  const int headingHeight{
      static_cast<int>(textExtent(settings.headingFont.get(), settings.heading.hwnd.get()).cy)};
  settings.heading.setPosition(left, y, right - left, headingHeight);
  y += headingHeight + px(8);
  rule(0);
  y += px(16) - std::max(1, px(1));

  // audio
  comboRow(settings.apiLabel, settings.api, width);
  comboRow(settings.outputLabel, settings.output, width);
  comboRow(settings.outputChannelsLabel, settings.outputChannels, width);
  comboRow(settings.inputLabel, settings.input, width);
  comboRow(settings.inputChannelsLabel, settings.inputChannels, width);
  noteRow(settings.inputNote);
  comboRow(settings.sampleRateLabel, settings.sampleRate, px(smallControlWidth));
  {
    const int rowTop{y};
    comboRow(settings.bufferSizeLabel, settings.bufferSize, px(smallControlWidth));
    if (settings.latencyNote.visible)
    {
      const int noteX{controlX + px(smallControlWidth) + px(columnSpacing)};
      const int rowHeight{y - px(rowSpacing) - rowTop};
      settings.latencyNote.setPosition(noteX, rowTop + (rowHeight - lineHeight) / 2, right - noteX,
                                       lineHeight);
    }
  }

  settings.resetToDefault.setPosition(controlX, y, buttonWidth(settings.resetToDefault),
                                      px(buttonHeight));
  // the driver's control panel, beside it, while there is one
  if (settings.driverPanel.visible)
  {
    settings.driverPanel.setPosition(controlX + buttonWidth(settings.resetToDefault) + px(buttonSpacing),
                                     y, buttonWidth(settings.driverPanel), px(buttonHeight));
  }
  y += px(buttonHeight) + px(rowSpacing);
  noteRow(settings.noRateNote);

  // midi
  if (takesMidi() || sendsMidi())
  {
    y -= px(rowSpacing);
    rule(px(rulePadding) + px(rowSpacing));
  }
  listRow(settings.midiInLabel, settings.midiIn);
  checkRow(settings.virtualIn, settings.virtualInLabel);
  listRow(settings.midiOutLabel, settings.midiOut);
  checkRow(settings.virtualOut, settings.virtualOutLabel);

  // appearance
  if (settings.appearance.visible)
  {
    y -= px(rowSpacing);
    rule(px(rulePadding) + px(rowSpacing));
  }
  comboRow(settings.appearanceLabel, settings.appearance, px(smallControlWidth));

  // buttons, OK and Cancel alike wide
  y += px(buttonsTop) - px(rowSpacing);
  const int buttons{std::max(buttonWidth(settings.ok), buttonWidth(settings.cancel))};
  settings.cancel.setPosition(right - buttons, y, buttons, px(buttonHeight));
  settings.ok.setPosition(right - 2 * buttons - px(buttonSpacing), y, buttons, px(buttonHeight));
  y += px(buttonHeight) + px(windowMargin);

  settings.adjustSize(right + px(windowMargin), y);
  settings.repaint();
}

void Plugin::selectApiInSettings()
{
  auto index{settings.api.selection(settings.apisShown.size())};
  if (!index || settings.apisShown[*index].id == sah->audioApiName) return;

  // another api's devices can only be listed by switching to it, which stops the stream:
  // the one thing the window does before OK. OK opens a stream on it, and Cancel goes
  // back to the api, and the devices, it opened with
  sah->setAudioApi(settings.apisShown[*index].id);
  settings.apiChanged = true;

  // its default devices; the input stays off if it was, and if the default is a
  // microphone, which no default opens (StandaloneHost::defaultInputToOpen)
  auto [input, output, rate] = sah->getDefaultAudioInOutSampleRate();
  const auto openable{sah->defaultInputToOpen()};
  refreshDevices(output, openable, shownInput().has_value() && !openable.empty());
  refreshChannelChoices(0, 0);
  refreshSampleRates(rate);
  refreshBufferSizes(shownBufferSize());
  refreshInputNote();
  refreshDriverPanelButton();
  applyShownAudio();
}

void Plugin::resetSettingsToDefault()
{
  auto [input, output, rate] = sah->getDefaultAudioInOutSampleRate();

  // a default never opens a microphone (StandaloneHost::defaultInputToOpen), so neither
  // does this: "No Input", and the note says why
  const auto openable{sah->defaultInputToOpen()};
  refreshDevices(output, openable, !openable.empty());
  refreshChannelChoices(0, 0);
  refreshSampleRates(rate);
  refreshBufferSizes(StandaloneSettings::defaultBufferSize);
  refreshInputNote();
  refreshDriverPanelButton();
  applyShownAudio();
}

// a stream on what the window shows, unless one runs on it already. false, with the
// previous stream back and the window left up for another choice, if it does not open
bool Plugin::openShownAudio()
{
  const auto out{shownOutput()};
  const auto in{shownInput()};
  const bool useOut{out.has_value()};
  const bool useIn{in.has_value()};
  const std::string outId{useOut ? settings.outDevices[*out].id : std::string{}};
  const std::string inId{useIn ? settings.inDevices[*in].id : std::string{}};
  const auto outChoice{shownChannels(false)};
  const auto inChoice{shownChannels(true)};
  const auto rate{shownSampleRate()};
  const auto frames{shownBufferSize()};

  // the stream running now, to go back to if the new one cannot be opened
  const bool wasRunning{sah->isAudioRunning()};
  const auto previousIn{sah->audioInputDevice};
  const auto previousOut{sah->audioOutputDevice};
  const bool previousUseIn{sah->audioInputUsed};
  const bool previousUseOut{sah->audioOutputUsed};
  const AudioChannelChoice previousInChannels{
      sah->deviceInputFirstChannel, sah->deviceInputChannels, {}};
  const AudioChannelChoice previousOutChannels{
      sah->deviceOutputFirstChannel, sah->deviceOutputChannels, {}};
  const auto previousRate{sah->currentSampleRate};
  const auto previousFrames{sah->currentBufferSize};

  // a stream already running on what the window shows is left alone: reopening it, for
  // a window in which only the midi ports changed, would drop out the audio for nothing
  const bool audioAsShown{
      wasRunning && !settings.apiChanged && rate == previousRate &&
      (frames == 0 || frames == previousFrames) && useOut == (sah->currentOutputChannels > 0) &&
      (!useOut || (outId == previousOut && outChoice.first == previousOutChannels.first &&
                   outChoice.count == previousOutChannels.count)) &&
      useIn == (sah->currentInputChannels > 0) &&
      (!useIn || (inId == previousIn && inChoice.first == previousInChannels.first &&
                  inChoice.count == previousInChannels.count))};

  if (!audioAsShown)
  {
    // the size to ask for; the stream reads back what the device granted
    if (frames > 0) sah->currentBufferSize = frames;
    auto useChannels = [this](const AudioChannelChoice &input, const AudioChannelChoice &output)
    {
      sah->deviceInputFirstChannel = input.first;
      sah->deviceInputChannels = std::max<uint32_t>(input.count, 1);
      sah->deviceOutputFirstChannel = output.first;
      sah->deviceOutputChannels = std::max<uint32_t>(output.count, 1);
    };
    useChannels(inChoice, outChoice);

    sah->startAudioThreadOn(inId, sah->deviceInputChannels, useIn, outId, sah->deviceOutputChannels,
                            useOut, rate);

    if (!sah->isAudioRunning())
    {
      // neither kept nor saved, and the window stays up for another choice: the devices
      // are put back as they were, and the error callback has already told the user why
      // these did not open
      sah->currentBufferSize = previousFrames;
      useChannels(previousInChannels, previousOutChannels);

      if (settings.apiChanged)
      {
        // the api the window opened with, which the saved settings name, and the window
        // shown again on it
        const auto appearanceWhenOpened{settings.appearanceWhenOpened};
        sah->applyAudioSettings();
        startAudio();
        settings.audioOpenedEarly = false;
        populateSettings();
        settings.appearanceWhenOpened = appearanceWhenOpened;
        layoutSettings();
      }
      else if (wasRunning)
      {
        sah->startAudioThreadOn(previousIn, sah->deviceInputChannels, previousUseIn, previousOut,
                                sah->deviceOutputChannels, previousUseOut, previousRate);
      }

      return false;
    }

    // what runs is no longer the saved setup, which Cancel goes back to; the api the
    // window opened with no longer runs either, whoever opened it
    settings.apiChanged = false;
    settings.audioOpenedEarly = true;
  }

  return true;
}

void Plugin::applyShownAudio()
{
  // nothing to open: no output, or devices without a rate in common, which OK refuses too
  if (shownOutput() && ::IsWindowEnabled(settings.ok.hwnd.get()))
  {
    if (openShownAudio())
    {
      // the size the device granted, which a driver may have chosen itself
      refreshBufferSizes(sah->currentBufferSize);
    }
    else
    {
      // the error box has said why; the menus show what runs again
      refreshDevices(sah->audioOutputDevice, sah->audioInputDevice, sah->audioInputUsed);
      refreshChannelChoices(sah->deviceOutputFirstChannel, sah->deviceInputFirstChannel);
      refreshSampleRates(sah->currentSampleRate);
      refreshBufferSizes(sah->currentBufferSize);
      refreshInputNote();
      refreshDriverPanelButton();
    }
  }
  layoutSettings();
}

void Plugin::okSettings()
{
  if (!sah || !::IsWindowEnabled(settings.ok.hwnd.get())) return;
  if (!openShownAudio()) return;

  const auto out{shownOutput()};
  const auto in{shownInput()};
  const bool useOut{out.has_value()};
  const bool useIn{in.has_value()};

  // persist the choice: startAudioThread() opens what the file names on the next launch.
  // only once it has opened, so a combination that cannot run is never what a launch
  // starts with; and by name, what the user chose, not what was opened, which may be a
  // fallback (standalone_settings.h)
  auto &saved{sah->settings};
  sah->setChosenDevices(useOut ? settings.outDevices[*out].id : std::string{},
                        useIn ? settings.inDevices[*in].id : std::string{});
  saved.audioOutputUsed = useOut;
  saved.audioInputUsed = useIn;
  if (useOut) saved.outputFirstChannel = shownChannels(false).first + 1;
  if (useIn) saved.inputFirstChannel = shownChannels(true).first + 1;
  saved.sampleRate = shownSampleRate();

  // a port list is applied only when it was changed, so that an untouched input list
  // keeps "every port" if that is what it had
  if (settings.midiIn.changed())
  {
    sah->setChosenMidiPorts(true, settings.midiIn.ticked());
    sah->openMidiPorts(settings.midiIn.ticked(), false);
  }
  if (settings.midiOut.changed())
  {
    sah->setChosenMidiPorts(false, settings.midiOut.ticked());
    sah->openMidiOutputPorts(settings.midiOut.ticked());
  }

  // likewise, so that an application connected to a virtual port keeps its connection
  const bool virtualIn{
      settings.virtualIn.message.send(settings.virtualIn.hwnd.get(), BM_GETCHECK, 0, 0) == BST_CHECKED};
  if (virtualIn != settings.virtualInWasOn)
  {
    saved.midiVirtualInput = virtualIn;
    sah->setVirtualMidiInput(virtualIn);
  }
  const bool virtualOut{settings.virtualOut.message.send(settings.virtualOut.hwnd.get(), BM_GETCHECK, 0,
                                                         0) == BST_CHECKED};
  if (virtualOut != settings.virtualOutWasOn)
  {
    saved.midiVirtualOutput = virtualOut;
    sah->setVirtualMidiOutput(virtualOut);
  }

  // the window's own choice, never a plugin's (StandaloneHost::screenAppearanceFromPlugin)
  if (!sah->screenAppearanceFromPlugin) saved.screenAppearance = sah->screenAppearance;

  if (!saveSettings()) log("Unable to save the audio settings; the next launch will not have them");

  settingsApplied = true;
  hideSettings();
}

// the driver's own settings, for the driver the window shows. its panel exists only while
// the driver is open, so a driver not open yet is opened first, on what the window shows;
// Cancel still goes back to the saved setup
void Plugin::showDriverControlPanel()
{
  if (!sah || !shownOutput()) return;
  if (!openShownAudio()) return;

  // a driver reset that its panel asks for is taken once the panel has returned: the
  // driver must not be closed from under its own call
  settings.inDriverPanel = true;
  if (!sah->showDriverControlPanel()) log("The audio driver has no control panel to show");
  settings.inDriverPanel = false;
}

void Plugin::cancelSettings()
{
  if (sah && !settingsApplied)
  {
    // an appearance tried in the window and not confirmed with OK goes back
    if (sah->screenAppearance != settings.appearanceWhenOpened)
      sah->setScreenAppearance(settings.appearanceWhenOpened, false);

    // a switched api stopped the stream, and the driver's control panel opened one on
    // what the window showed: back to the saved setup either way
    if (settings.apiChanged || settings.audioOpenedEarly)
    {
      settings.apiChanged = false;
      settings.audioOpenedEarly = false;
      sah->applyAudioSettings();
      startAudio();
    }
  }

  hideSettings();
}

// a direction the plugin has no note port for gets no section
bool Plugin::takesMidi() const
{
  return sah && (sah->hasMIDIInput || sah->hasClapNoteInput);
}

bool Plugin::sendsMidi() const
{
  return sah && sah->createsMidiOutput;
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

void Plugin::selectDefaultDevices()
{
  auto [input, output, sampleRate]{sah->getDefaultAudioInOutSampleRate()};

  // No device chosen under this API; empty means "follow the system default".
  // Devices from the previous API would not resolve here anyway.
  sah->settings.inputDeviceId.clear();
  sah->settings.inputDeviceName.clear();
  sah->settings.outputDeviceId.clear();
  sah->settings.outputDeviceName.clear();

  // The default input only if it names something real, and never a microphone
  // (StandaloneHost::defaultInputToOpen): with nothing chosen, there is no input then.
  // The probe is a fact about the machine, not a choice: AND it with the user's wish,
  // which survives an API switch, and never persist it.
  const auto openable{sah->defaultInputToOpen()};
  sah->audioInputDevice = openable.empty() ? input : openable;
  sah->audioInputUsed = sah->settings.audioInputUsed && !openable.empty();

  sah->audioOutputDevice = output;
  sah->audioOutputUsed = sah->settings.audioOutputUsed && sah->isKnownDevice(output);

  sah->currentSampleRate = sampleRate;
  sah->currentBufferSize = StandaloneSettings::defaultBufferSize;

  // each device's first choice of channels for the plugin's main ports
  const auto inChoice{sah->channelChoiceFor(sah->audioInputDevice, true, 0)};
  const auto outChoice{sah->channelChoiceFor(sah->audioOutputDevice, false, 0)};
  sah->deviceInputFirstChannel = inChoice.first;
  sah->deviceInputChannels = std::max<uint32_t>(inChoice.count, 1);
  sah->deviceOutputFirstChannel = outChoice.first;
  sah->deviceOutputChannels = std::max<uint32_t>(outChoice.count, 1);
}

void Plugin::applyLoadedAudio()
{
  // the devices and their channels as saved (StandaloneHost::applyAudioSettings)
  sah->applyAudioSettings();
}

void Plugin::applyDefaultAudio()
{
  // The default api, which is WASAPI: always present and always working on a stock
  // Windows, so it is what a first run gets, ASIO compiled in or not.
  sah->setAudioApi({});
  selectDefaultDevices();
}

// whether the next launch gets this session's state back; saved with the audio and midi setup
void Plugin::toggleRestoreLastSession()
{
  if (!sah) return;
  sah->setPluginStatePersistence(sah->restoresLastSession()
                                     ? CLAP_WRAPPER_STANDALONE_STATE_PERSIST_NONE
                                     : CLAP_WRAPPER_STANDALONE_STATE_PERSIST_SESSION);
  showRestoreLastSession(getSystemMenu(hwnd.get()));
}

void Plugin::showRestoreLastSession(::HMENU systemMenu)
{
  if (!sah || !systemMenu) return;
  ::CheckMenuItem(systemMenu, Menu::Identifier::RestoreLastSession,
                  MF_BYCOMMAND | (sah->restoresLastSession() ? MF_CHECKED : MF_UNCHECKED));
}

void Plugin::startAudio()
{
  sah->startAudioThreadOn(sah->audioInputDevice, sah->deviceInputChannels, sah->audioInputUsed,
                          sah->audioOutputDevice, sah->deviceOutputChannels, sah->audioOutputUsed,
                          sah->currentSampleRate);
}
}  // namespace freeaudio::clap_wrapper::standalone::windows_standalone
