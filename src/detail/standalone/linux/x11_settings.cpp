#include "x11_settings.h"

#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdio>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
namespace
{
// the measures the macOS and Windows windows use
constexpr int margin{20}, headingTop{16}, spacing{8}, controlWidth{320}, smallWidth{120};
constexpr int buttonMinWidth{80}, buttonPadding{12}, aboveButtons{24};
constexpr size_t visiblePortRows{5};
constexpr int menuRowsShown{12};

std::string formatMs(uint32_t frames, uint32_t rate)
{
  char text[32];
  snprintf(text, sizeof(text), "%.1f", 1000.0 * frames / rate);
  return text;
}
}  // namespace

SettingsWindow::SettingsWindow(Display *d, StandaloneHost *h, const UiFont &f, Window transientFor)
  : display(d), host(h), font(f)
{
  theme = Theme::make(display, host->screenAppearance == CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK);

  // what Cancel goes back to
  audioWhenOpened = runningSetup();
  apiWhenOpened = host->audioApiName;
  appearanceWhenOpened = host->screenAppearance;
  midiInAllOn = host->settings.midiBindAllPorts;
  virtualInWasOn = host->settings.midiVirtualInput;
  virtualOutWasOn = host->settings.midiVirtualOutput;

  build();
  virtualIn.checked = virtualInWasOn;
  virtualOut.checked = virtualOutWasOn;
  fillApis();
  fillDevices(host->audioOutputDevice, host->audioInputUsed ? host->audioInputDevice : std::string{});
  fillChannels(host->deviceOutputFirstChannel, host->deviceInputFirstChannel);
  fillRates(static_cast<uint32_t>(std::max(host->currentSampleRate, 0)));
  fillBufferSizes(host->currentBufferSize);
  updateInputNote();
  fillMidi();
  fillAppearance();
  layout();

  const auto screen{DefaultScreen(display)};
  window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, static_cast<unsigned>(width),
                               static_cast<unsigned>(height), 0, theme.background, theme.background);
  XStoreName(display, window, "Audio/MIDI Settings");
  XSelectInput(display, window,
               ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask |
                   LeaveWindowMask | StructureNotifyMask);
  wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(display, window, &wmDelete, 1);
  if (transientFor) XSetTransientForHint(display, window, transientFor);
  gc = XCreateGC(display, window, 0, nullptr);

  // a fixed size, which layout() sets again whenever rows come or go
  layout();
  XMapRaised(display, window);
}

SettingsWindow::~SettingsWindow()
{
  if (popup) XDestroyWindow(display, popup);
  if (gc) XFreeGC(display, gc);
  if (window) XDestroyWindow(display, window);
  XFlush(display);
}

void SettingsWindow::raise()
{
  XMapRaised(display, window);
  XSetInputFocus(display, window, RevertToParent, CurrentTime);
}

// ---------------------------------------------------------------------------------------
// the controls

void SettingsWindow::build()
{
  using Kind = Control::Kind;
  auto label = [](Control &c, const char *text)
  {
    c.kind = Kind::Label;
    c.text = text;
  };
  auto menu = [](Control &c, std::function<void()> onChange)
  {
    c.kind = Kind::Menu;
    c.onChange = std::move(onChange);
  };

  std::string name{"the plugin"};
  if (host->clapPlugin && host->clapPlugin->_plugin && host->clapPlugin->_plugin->desc &&
      host->clapPlugin->_plugin->desc->name)
    name = host->clapPlugin->_plugin->desc->name;
  heading.kind = Kind::Heading;
  heading.text = "Audio/MIDI Setup for " + name;
  headingRule.kind = Kind::Rule;

  label(apiLabel, "Audio API:");
  menu(api, [this] { onApiChanged(); });
  label(outputLabel, "Output:");
  menu(output, [this] { onDeviceChanged(); });
  label(outputChannelsLabel, "Output Channels:");
  menu(outputChannels, [this] { openShownAudio(); });
  label(inputLabel, "Input:");
  menu(input, [this] { onDeviceChanged(); });
  label(inputChannelsLabel, host->mainBusChannels(true) == 1 ? "Input Channel:" : "Input Channels:");
  menu(inputChannels, [this] { openShownAudio(); });
  inputNote.kind = Kind::Note;
  label(rateLabel, "Sample Rate:");
  menu(rate, [this] { onRateChanged(); });
  label(bufferLabel, "Buffer Size:");
  menu(buffer, [this] { openShownAudio(); });
  latencyNote.kind = Kind::Note;
  defaults.kind = Kind::Button;
  defaults.text = "Reset to System Default";
  defaults.onChange = [this] { onDefaults(); };
  noRateNote.kind = Kind::Note;
  noRateNote.text = "These devices have no sample rate in common.";
  errorNote.kind = Kind::Note;

  midiRule.kind = Kind::Rule;
  label(midiInLabel, "MIDI Inputs:");
  midiIn.kind = Kind::PortList;
  midiIn.emptyText = "No MIDI inputs found";
  virtualIn.kind = Kind::Check;
  virtualIn.text = "Virtual input \xE2\x80\x9C" + host->virtualMidiPortName(true) + "\xE2\x80\x9D";
  label(midiOutLabel, "MIDI Outputs:");
  midiOut.kind = Kind::PortList;
  midiOut.emptyText = "No MIDI outputs found";
  virtualOut.kind = Kind::Check;
  virtualOut.text = "Virtual output \xE2\x80\x9C" + host->virtualMidiPortName(false) + "\xE2\x80\x9D";

  appearanceRule.kind = Kind::Rule;
  label(appearanceLabel, "Appearance:");
  menu(appearance, [this] { onAppearanceChanged(); });

  cancelButton.kind = Kind::Button;
  cancelButton.text = "Cancel";
  cancelButton.onChange = [this] { close(); };
  okButton.kind = Kind::Button;
  okButton.text = "OK";
  okButton.onChange = [this] { ok(); };

  order = {&heading,
           &headingRule,
           &apiLabel,
           &api,
           &outputLabel,
           &output,
           &outputChannelsLabel,
           &outputChannels,
           &inputLabel,
           &input,
           &inputChannelsLabel,
           &inputChannels,
           &inputNote,
           &rateLabel,
           &rate,
           &bufferLabel,
           &buffer,
           &latencyNote,
           &defaults,
           &noRateNote,
           &errorNote,
           &midiRule,
           &midiInLabel,
           &midiIn,
           &virtualIn,
           &midiOutLabel,
           &midiOut,
           &virtualOut,
           &appearanceRule,
           &appearanceLabel,
           &appearance,
           &cancelButton,
           &okButton};
}

// right-aligned labels in a column as wide as the widest shown, the controls beside them
void SettingsWindow::layout()
{
  const int rowHeight{std::max(font.height() + 10, 23)};
  const int portRowHeight{font.height() + 6};

  // which rows are there
  api.visible = apiLabel.visible = apis.size() > 1;
  inputChannelsLabel.visible = inputChannels.visible;
  latencyNote.visible = !latencyNote.text.empty();
  inputNote.visible = !inputNote.text.empty();
  errorNote.visible = !errorNote.text.empty();
  midiRule.visible = takesMidi || sendsMidi;
  midiIn.visible = midiInLabel.visible = takesMidi;
  midiOut.visible = midiOutLabel.visible = sendsMidi;
  virtualIn.visible = takesMidi && host->supportsVirtualMidiPorts();
  virtualOut.visible = sendsMidi && host->supportsVirtualMidiPorts();
  appearanceRule.visible = appearanceLabel.visible = appearance.visible;

  int labelWidth{0};
  for (auto *c : order)
  {
    if (c->kind == Control::Kind::Label && c->visible)
      labelWidth = std::max(labelWidth, font.width(c->text));
  }
  const int cx{margin + labelWidth + spacing};
  const int right{cx + controlWidth};

  int y{headingTop};
  heading.rect = {margin, y, right - margin, font.height()};
  y += font.height() + 6;
  headingRule.rect = {margin, y, right - margin, 1};
  y += 1 + 16;

  auto row = [&](Control &label, Control &control, int w)
  {
    if (!control.visible) return;
    control.rect = {cx, y, w, rowHeight};
    label.rect = {margin, y, labelWidth, rowHeight};
    y += rowHeight + spacing;
  };
  auto note = [&](Control &n)
  {
    if (!n.visible) return;
    const auto lines{font.wrap(n.text, controlWidth)};
    n.rect = {cx, y, controlWidth, static_cast<int>(lines.size()) * font.height()};
    y += n.rect.h + spacing;
  };
  auto rule = [&](Control &r)
  {
    if (!r.visible) return;
    y += 6;
    r.rect = {margin, y, right - margin, 1};
    y += 1 + 6 + spacing;
  };
  auto list = [&](Control &label, Control &l)
  {
    if (!l.visible) return;
    const auto rows{std::max<size_t>(l.rows.size(), 1)};
    const bool scrolls{l.rows.size() > visiblePortRows};
    const double shown{static_cast<double>(std::min(rows, visiblePortRows)) + (scrolls ? 0.5 : 0)};
    l.rect = {cx, y, controlWidth, 8 + static_cast<int>(shown * portRowHeight)};
    label.rect = {margin, y + 4, labelWidth, portRowHeight};
    y += l.rect.h + spacing;
  };
  auto check = [&](Control &c)
  {
    if (!c.visible) return;
    c.rect = {cx, y, controlWidth, rowHeight - 4};
    y += c.rect.h + spacing;
  };

  row(apiLabel, api, controlWidth);
  row(outputLabel, output, controlWidth);
  row(outputChannelsLabel, outputChannels, controlWidth);
  row(inputLabel, input, controlWidth);
  row(inputChannelsLabel, inputChannels, controlWidth);
  note(inputNote);
  if (latencyNote.visible)
  {
    latencyNote.rect = {cx + smallWidth + spacing, y + (rowHeight - font.height()) / 2,
                        controlWidth - smallWidth - spacing, font.height()};
  }
  row(rateLabel, rate, smallWidth);
  if (latencyNote.visible) latencyNote.rect.y = y + (rowHeight - font.height()) / 2;
  row(bufferLabel, buffer, smallWidth);
  defaults.rect = {cx, y, std::max(buttonMinWidth, font.width(defaults.text) + 2 * buttonPadding),
                   rowHeight};
  y += rowHeight + spacing;
  note(noRateNote);
  note(errorNote);

  rule(midiRule);
  list(midiInLabel, midiIn);
  check(virtualIn);
  list(midiOutLabel, midiOut);
  check(virtualOut);

  rule(appearanceRule);
  row(appearanceLabel, appearance, smallWidth);

  y += aboveButtons - spacing;
  const int buttonWidth{std::max({buttonMinWidth, font.width(okButton.text) + 2 * buttonPadding,
                                  font.width(cancelButton.text) + 2 * buttonPadding})};
  okButton.rect = {right - buttonWidth, y, buttonWidth, rowHeight};
  cancelButton.rect = {right - 2 * buttonWidth - spacing, y, buttonWidth, rowHeight};
  y += rowHeight + margin;

  const int w{right + margin}, h{y};
  if (window && (w != width || h != height))
  {
    XResizeWindow(display, window, static_cast<unsigned>(w), static_cast<unsigned>(h));
    XSizeHints *hints{XAllocSizeHints()};
    hints->flags = PMinSize | PMaxSize;
    hints->min_width = hints->max_width = w;
    hints->min_height = hints->max_height = h;
    XSetWMNormalHints(display, window, hints);
    XFree(hints);
  }
  width = w;
  height = h;
  if (window) draw();
}

// ---------------------------------------------------------------------------------------
// drawing

void SettingsWindow::draw()
{
  if (!window) return;
  // into a pixmap first, so that a redraw does not flicker
  auto pixmap{XCreatePixmap(display, window, static_cast<unsigned>(width), static_cast<unsigned>(height),
                            static_cast<unsigned>(DefaultDepth(display, DefaultScreen(display))))};
  Painter p{display, pixmap, gc, &font};
  p.fill({0, 0, width, height}, theme.background);
  for (auto *c : order)
  {
    if (c->visible) drawControl(p, *c, c == hot);
  }
  XCopyArea(display, pixmap, window, gc, 0, 0, static_cast<unsigned>(width),
            static_cast<unsigned>(height), 0, 0);
  XFreePixmap(display, pixmap);
  XFlush(display);
}

void SettingsWindow::drawControl(const Painter &p, const Control &c, bool isHot)
{
  using Kind = Control::Kind;
  const auto textColor{c.enabled ? theme.text : theme.disabled};
  const auto &r{c.rect};
  switch (c.kind)
  {
    case Kind::Label:
    {
      const auto w{font.width(c.text)};
      p.text(r.x + r.w - w, r.y + (r.h - font.height()) / 2, c.text, theme.text);
      break;
    }
    case Kind::Heading:
      p.text(r.x, r.y, font.fit(c.text, r.w), theme.text);
      break;
    case Kind::Note:
    {
      int y{r.y};
      for (const auto &line : font.wrap(c.text, r.w))
      {
        p.text(r.x, y, line, c.colored ? c.color : theme.secondary);
        y += font.height();
      }
      break;
    }
    case Kind::Rule:
      p.line(r.x, r.y, r.x + r.w, r.y, theme.rule);
      break;
    case Kind::Menu:
    {
      p.fill(r, isHot && c.enabled ? theme.hover : theme.field);
      p.frame(r, theme.fieldBorder);
      const auto title{c.selected >= 0 && c.selected < static_cast<int>(c.items.size())
                           ? c.items[c.selected]
                           : std::string{}};
      p.textIn({r.x, r.y, r.w - 20, r.h}, 8, title, textColor);
      p.chevron(r.x + r.w - 12, r.y + r.h / 2, textColor);
      break;
    }
    case Kind::Check:
    {
      const int box{13};
      const Rect b{r.x, r.y + (r.h - box) / 2, box, box};
      p.fill(b, c.checked ? theme.accent : theme.field);
      p.frame(b, c.checked ? theme.accent : theme.fieldBorder);
      if (c.checked) p.tick(b, theme.accentText);
      p.textIn({r.x + box + 6, r.y, r.w - box - 6, r.h}, 0, c.text, textColor);
      break;
    }
    case Kind::Button:
    {
      const bool primary{&c == &okButton && c.enabled};
      p.fill(r, primary ? theme.accent : (isHot && c.enabled ? theme.hover : theme.field));
      p.frame(r, primary ? theme.accent : theme.fieldBorder);
      const auto w{font.width(c.text)};
      p.text(r.x + (r.w - w) / 2, r.y + (r.h - font.height()) / 2, c.text,
             primary ? theme.accentText : textColor);
      break;
    }
    case Kind::PortList:
    {
      p.fill(r, theme.field);
      p.frame(r, theme.fieldBorder);
      const int rowHeight{font.height() + 6};
      if (c.rows.empty())
      {
        p.text(r.x + 6, r.y + 4 + 3, c.emptyText, theme.secondary);
        break;
      }
      // clipped to the box: rows are drawn into it and scroll inside it
      XRectangle clip{static_cast<short>(r.x + 1), static_cast<short>(r.y + 1),
                      static_cast<unsigned short>(r.w - 2), static_cast<unsigned short>(r.h - 2)};
      XSetClipRectangles(display, gc, 0, 0, &clip, 1, Unsorted);
      int y{r.y + 4 - c.scroll * rowHeight};
      for (const auto &row : c.rows)
      {
        const int box{13};
        const Rect b{r.x + 6, y + (rowHeight - box) / 2, box, box};
        p.fill(b, row.on ? theme.accent : theme.field);
        p.frame(b, row.on ? theme.accent : theme.fieldBorder);
        if (row.on) p.tick(b, theme.accentText);
        p.textIn({b.x + box + 6, y, r.w - box - 22, rowHeight}, 0,
                 row.connected ? row.title : row.title + " (not connected)",
                 row.connected ? theme.text : theme.secondary);
        y += rowHeight;
      }
      XSetClipMask(display, gc, None);
      // a list that scrolls says so with a thin bar
      const int shown{(r.h - 8) / rowHeight};
      if (static_cast<int>(c.rows.size()) > shown)
      {
        const int total{static_cast<int>(c.rows.size())};
        const int barHeight{std::max(12, (r.h - 4) * shown / total)};
        const int barTop{r.y + 2 + (r.h - 4 - barHeight) * c.scroll / std::max(1, total - shown)};
        p.fill({r.x + r.w - 5, barTop, 3, barHeight}, theme.fieldBorder);
      }
      break;
    }
  }
}

void SettingsWindow::drawPopup()
{
  if (!popup || !openedMenu) return;
  const int rowHeight{font.height() + 8};
  const int w{popupRect.w}, h{popupRect.h};
  auto pixmap{XCreatePixmap(display, popup, static_cast<unsigned>(w), static_cast<unsigned>(h),
                            static_cast<unsigned>(DefaultDepth(display, DefaultScreen(display))))};
  Painter p{display, pixmap, gc, &font};
  p.fill({0, 0, w, h}, theme.field);
  p.frame({0, 0, w, h}, theme.fieldBorder);
  const auto &items{openedMenu->items};
  for (int i = 0; i < popupRows; ++i)
  {
    const int index{popupScroll + i};
    if (index >= static_cast<int>(items.size())) break;
    const Rect row{1, 1 + i * rowHeight, w - 2, rowHeight};
    const bool selected{index == openedMenu->selected};
    if (index == popupHot) p.fill(row, theme.accent);
    if (selected && index != popupHot) p.fill({row.x, row.y, 3, row.h}, theme.accent);
    p.textIn(row, 10, items[static_cast<size_t>(index)],
             index == popupHot ? theme.accentText : theme.text);
  }
  if (static_cast<int>(items.size()) > popupRows)
  {
    const int total{static_cast<int>(items.size())};
    const int barHeight{std::max(12, (h - 4) * popupRows / total)};
    const int barTop{2 + (h - 4 - barHeight) * popupScroll / std::max(1, total - popupRows)};
    p.fill({w - 5, barTop, 3, barHeight}, theme.fieldBorder);
  }
  XCopyArea(display, pixmap, popup, gc, 0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h), 0, 0);
  XFreePixmap(display, pixmap);
  XFlush(display);
}

// ---------------------------------------------------------------------------------------
// events

SettingsWindow::Control *SettingsWindow::hit(int x, int y)
{
  for (auto *c : order)
  {
    if (!c->visible || !c->enabled) continue;
    switch (c->kind)
    {
      case Control::Kind::Menu:
      case Control::Kind::Check:
      case Control::Kind::Button:
      case Control::Kind::PortList:
        if (c->rect.contains(x, y)) return c;
        break;
      default:
        break;
    }
  }
  return nullptr;
}

void SettingsWindow::handle(const XEvent &event)
{
  if (closedWith) return;

  if (event.xany.window == popup)
  {
    const int rowHeight{font.height() + 8};
    switch (event.type)
    {
      case Expose:
        if (event.xexpose.count == 0) drawPopup();
        break;
      case MotionNotify:
      {
        const auto &m{event.xmotion};
        int hotNow{-1};
        if (m.x >= 0 && m.y >= 0 && m.x < popupRect.w && m.y < popupRect.h)
          hotNow = std::min(popupScroll + (m.y - 1) / rowHeight,
                            static_cast<int>(openedMenu->items.size()) - 1);
        if (hotNow != popupHot)
        {
          popupHot = hotNow;
          drawPopup();
        }
        break;
      }
      case ButtonPress:
      {
        const auto &b{event.xbutton};
        const int scrollMax{std::max(0, static_cast<int>(openedMenu->items.size()) - popupRows)};
        if (b.button == Button4 || b.button == Button5)
        {
          popupScroll = std::clamp(popupScroll + (b.button == Button4 ? -1 : 1), 0, scrollMax);
          drawPopup();
          break;
        }
        const bool inside{b.x >= 0 && b.y >= 0 && b.x < popupRect.w && b.y < popupRect.h};
        if (inside)
          popupHot = std::min(popupScroll + (b.y - 1) / rowHeight,
                              static_cast<int>(openedMenu->items.size()) - 1);
        closeMenu(inside);
        break;
      }
      case KeyPress:
      {
        auto key{XLookupKeysym(const_cast<XKeyEvent *>(&event.xkey), 0)};
        const int last{static_cast<int>(openedMenu->items.size()) - 1};
        if (key == XK_Escape)
          closeMenu(false);
        else if (key == XK_Return || key == XK_KP_Enter || key == XK_space)
          closeMenu(popupHot >= 0);
        else if (key == XK_Up || key == XK_Down)
        {
          popupHot = std::clamp(popupHot + (key == XK_Up ? -1 : 1), 0, last);
          if (popupHot < popupScroll) popupScroll = popupHot;
          if (popupHot >= popupScroll + popupRows) popupScroll = popupHot - popupRows + 1;
          drawPopup();
        }
        break;
      }
      default:
        break;
    }
    return;
  }

  switch (event.type)
  {
    case Expose:
      if (event.xexpose.count == 0) draw();
      break;
    case MotionNotify:
    {
      auto *now{hit(event.xmotion.x, event.xmotion.y)};
      if (now != hot)
      {
        hot = now;
        draw();
      }
      break;
    }
    case LeaveNotify:
      if (hot)
      {
        hot = nullptr;
        draw();
      }
      break;
    case ButtonPress:
    {
      const auto &b{event.xbutton};
      auto *c{hit(b.x, b.y)};
      if (b.button == Button4 || b.button == Button5)
      {
        if (c && c->kind == Control::Kind::PortList) scrollList(*c, b.button == Button4 ? -1 : 1);
        break;
      }
      if (b.button != Button1) break;
      pressed = c;
      // a menu opens on the press, as menus do
      if (c && c->kind == Control::Kind::Menu)
      {
        pressed = nullptr;
        openMenu(*c);
      }
      break;
    }
    case ButtonRelease:
    {
      const auto &b{event.xbutton};
      if (b.button != Button1) break;
      auto *c{hit(b.x, b.y)};
      if (c && c == pressed) click(*c, b.x, b.y);
      pressed = nullptr;
      break;
    }
    case KeyPress:
    {
      auto key{XLookupKeysym(const_cast<XKeyEvent *>(&event.xkey), 0)};
      if ((key == XK_Return || key == XK_KP_Enter) && okButton.enabled)
        ok();
      else if (key == XK_Escape)
        close();
      break;
    }
    case ClientMessage:
      if (static_cast<Atom>(event.xclient.data.l[0]) == wmDelete) close();
      break;
    default:
      break;
  }
}

void SettingsWindow::click(Control &c, int x, int y)
{
  switch (c.kind)
  {
    case Control::Kind::Button:
      if (c.onChange) c.onChange();
      break;
    case Control::Kind::Check:
      c.checked = !c.checked;
      draw();
      break;
    case Control::Kind::PortList:
    {
      const int rowHeight{font.height() + 6};
      const int index{c.scroll + (y - c.rect.y - 4) / rowHeight};
      if (index >= 0 && index < static_cast<int>(c.rows.size()) && y >= c.rect.y + 4)
      {
        auto &row{c.rows[static_cast<size_t>(index)]};
        row.on = !row.on;
        draw();
      }
      (void)x;
      break;
    }
    default:
      break;
  }
}

void SettingsWindow::scrollList(Control &list, int by)
{
  const int rowHeight{font.height() + 6};
  const int shown{(list.rect.h - 8) / rowHeight};
  const int scrollMax{std::max(0, static_cast<int>(list.rows.size()) - shown)};
  list.scroll = std::clamp(list.scroll + by, 0, scrollMax);
  draw();
}

void SettingsWindow::openMenu(Control &menu)
{
  if (menu.items.empty()) return;
  const int rowHeight{font.height() + 8};
  popupRows = std::min(static_cast<int>(menu.items.size()), menuRowsShown);
  popupScroll = std::clamp(menu.selected - popupRows / 2, 0,
                           std::max(0, static_cast<int>(menu.items.size()) - popupRows));
  popupHot = menu.selected;

  int width{menu.rect.w};
  for (const auto &item : menu.items) width = std::max(width, std::min(font.width(item) + 30, 520));
  const int height{popupRows * rowHeight + 2};

  // below the menu, or above it where the screen ends
  int rootX{0}, rootY{0};
  Window child{0};
  XTranslateCoordinates(display, window, DefaultRootWindow(display), menu.rect.x,
                        menu.rect.y + menu.rect.h, &rootX, &rootY, &child);
  const int screenHeight{DisplayHeight(display, DefaultScreen(display))};
  if (rootY + height > screenHeight) rootY = std::max(0, rootY - menu.rect.h - height);
  popupRect = {rootX, rootY, width, height};

  XSetWindowAttributes attributes{};
  attributes.override_redirect = True;
  attributes.background_pixel = theme.field;
  attributes.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | KeyPressMask;
  popup = XCreateWindow(display, DefaultRootWindow(display), rootX, rootY, static_cast<unsigned>(width),
                        static_cast<unsigned>(height), 0, CopyFromParent, InputOutput, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &attributes);
  openedMenu = &menu;
  XMapRaised(display, popup);
  // every click goes to the menu while it is open: one outside it closes it
  XGrabPointer(display, popup, False, ButtonPressMask | PointerMotionMask, GrabModeAsync, GrabModeAsync,
               None, None, CurrentTime);
  XGrabKeyboard(display, popup, False, GrabModeAsync, GrabModeAsync, CurrentTime);
  drawPopup();
}

void SettingsWindow::closeMenu(bool choose)
{
  if (!popup) return;
  XUngrabPointer(display, CurrentTime);
  XUngrabKeyboard(display, CurrentTime);
  XDestroyWindow(display, popup);
  popup = 0;
  auto *menu{openedMenu};
  openedMenu = nullptr;
  if (choose && menu && popupHot >= 0 && popupHot != menu->selected)
  {
    menu->selected = popupHot;
    // a choice starts over: the error of the one before is of no more use
    errorNote.text.clear();
    if (menu->onChange) menu->onChange();
    layout();
  }
  draw();
}

// ---------------------------------------------------------------------------------------
// what the menus show, from the host

void SettingsWindow::fillApis()
{
  apis = host->getAudioApis();
  api.items.clear();
  api.selected = 0;
  for (size_t i = 0; i < apis.size(); ++i)
  {
    api.items.push_back(apis[i].name);
    if (apis[i].id == host->audioApiName) api.selected = static_cast<int>(i);
  }
}

void SettingsWindow::fillDevices(const std::string &outputId, const std::string &inputId)
{
  outDevices = host->getOutputAudioDevices();
  inDevices = host->getInputAudioDevices();
  auto titles = [](const std::vector<AudioDevice> &devices)
  {
    std::vector<const AudioDevice *> pointers;
    for (const auto &d : devices) pointers.push_back(&d);
    return deviceMenuTitles(pointers);
  };

  output.items = titles(outDevices);
  output.selected = outDevices.empty() ? -1 : 0;
  for (size_t i = 0; i < outDevices.size(); ++i)
  {
    if (outDevices[i].id == outputId) output.selected = static_cast<int>(i);
  }

  input.items = {"No Input"};
  input.selected = 0;
  const auto inTitles{titles(inDevices)};
  for (size_t i = 0; i < inDevices.size(); ++i)
  {
    input.items.push_back(inTitles[i]);
    if (!inputId.empty() && inDevices[i].id == inputId) input.selected = static_cast<int>(i) + 1;
  }
}

const AudioDevice *SettingsWindow::shownDevice(bool in) const
{
  if (in)
    return input.selected > 0 && static_cast<size_t>(input.selected) <= inDevices.size()
               ? &inDevices[static_cast<size_t>(input.selected) - 1]
               : nullptr;
  return output.selected >= 0 && static_cast<size_t>(output.selected) < outDevices.size()
             ? &outDevices[static_cast<size_t>(output.selected)]
             : nullptr;
}

void SettingsWindow::fillChannels(uint32_t outputFirst, uint32_t inputFirst)
{
  auto fill = [](Control &menu, std::vector<AudioChannelChoice> &choices,
                 std::vector<AudioChannelChoice> from, uint32_t first)
  {
    choices = std::move(from);
    menu.items.clear();
    menu.selected = choices.empty() ? -1 : 0;
    for (size_t i = 0; i < choices.size(); ++i)
    {
      menu.items.push_back(choices[i].title);
      if (choices[i].first == first) menu.selected = static_cast<int>(i);
    }
    menu.enabled = choices.size() > 1;
  };
  const auto *out{shownDevice(false)};
  const auto *in{shownDevice(true)};
  fill(outputChannels, outChoices,
       out ? host->getChannelChoices(out->id, false) : std::vector<AudioChannelChoice>{}, outputFirst);
  fill(inputChannels, inChoices,
       in ? host->getChannelChoices(in->id, true) : std::vector<AudioChannelChoice>{}, inputFirst);
  inputChannels.visible = in != nullptr && host->numAudioInputs > 0;
}

AudioChannelChoice SettingsWindow::shownChoice(bool in) const
{
  const auto &choices{in ? inChoices : outChoices};
  const auto index{(in ? inputChannels : outputChannels).selected};
  if (index < 0 || static_cast<size_t>(index) >= choices.size()) return {};
  return choices[static_cast<size_t>(index)];
}

void SettingsWindow::fillRates(uint32_t preferred)
{
  rate.items.clear();
  rate.tags.clear();
  rate.selected = -1;
  const auto *out{shownDevice(false)};
  if (!out)
  {
    // no output device at all: nothing to run a stream on
    okButton.enabled = false;
    noRateNote.visible = false;
    return;
  }
  const auto rates{commonSampleRates(out, shownDevice(true))};
  const auto selected{preferredSampleRate(rates, preferred)};
  for (size_t i = 0; i < rates.size(); ++i)
  {
    rate.items.push_back(std::to_string(rates[i]));
    rate.tags.push_back(rates[i]);
    if (rates[i] == selected) rate.selected = static_cast<int>(i);
  }
  // no rate in common, nothing to open the two devices at
  okButton.enabled = !rates.empty();
  noRateNote.visible = rates.empty();
}

uint32_t SettingsWindow::shownRate() const
{
  return rate.selected >= 0 ? static_cast<uint32_t>(rate.tags[static_cast<size_t>(rate.selected)]) : 0;
}

uint32_t SettingsWindow::shownFrames() const
{
  return buffer.selected >= 0 ? static_cast<uint32_t>(buffer.tags[static_cast<size_t>(buffer.selected)])
                              : 0;
}

void SettingsWindow::fillBufferSizes(uint32_t preferred)
{
  const auto *out{shownDevice(false)};
  const auto *in{shownDevice(true)};
  auto sizes{host->getBufferSizesFor(out ? out->id : std::string{}, in ? in->id : std::string{})};

  // the stream may run at a size the list does not have, which the device chose
  const auto running{host->currentBufferSize};
  if (running > 0 && std::find(sizes.begin(), sizes.end(), running) == sizes.end())
    sizes.insert(std::upper_bound(sizes.begin(), sizes.end(), running), running);

  if (preferred == 0) preferred = StandaloneSettings::defaultBufferSize;
  buffer.tags.assign(sizes.begin(), sizes.end());
  buffer.selected = sizes.empty() ? -1 : 0;
  for (size_t i = 0; i < sizes.size(); ++i)
  {
    if (sizes[i] <= preferred) buffer.selected = static_cast<int>(i);
  }
  updateBufferTitles();
}

// each size with how long it lasts at the selected rate: "256 (5.8 ms)"
void SettingsWindow::updateBufferTitles()
{
  const auto r{shownRate()};
  buffer.items.clear();
  for (auto frames : buffer.tags)
  {
    const auto f{static_cast<uint32_t>(frames)};
    buffer.items.push_back(r > 0 ? std::to_string(f) + " (" + formatMs(f, r) + " ms)"
                                 : std::to_string(f));
  }
  updateLatencyNote();
}

void SettingsWindow::updateInputNote()
{
  inputNote.text.clear();
  inputNote.colored = false;
  const auto *in{shownDevice(true)};
  if (in && in->isBluetooth)
  {
    inputNote.text =
        "A Bluetooth microphone switches its device to telephone quality, for every application, while "
        "it is open.";
    inputNote.color = theme.caution;
    inputNote.colored = true;
  }
  else if (!in && host->numAudioInputs > 0)
  {
    auto [defaultIn, defaultOut, defaultRate] = host->getDefaultAudioInOutSampleRate();
    const auto *device{host->findDevice(defaultIn)};
    if (device && device->isMicrophone)
      inputNote.text = "The default input is a microphone, which is opened only when chosen here.";
  }
}

// "Latency: in 5.3 ms, out 30.0 ms" for the running stream, while the window shows its
// devices, channels, rate and size. not shown when the numbers are not known
void SettingsWindow::updateLatencyNote()
{
  const auto &stream{host->currentStream};
  const auto *out{shownDevice(false)};
  const auto *in{shownDevice(true)};
  const std::string outId{out ? out->id : std::string{}}, inId{in ? in->id : std::string{}};
  const bool showsRunning{host->isAudioRunning() && !apiChanged && stream.sampleRate > 0 &&
                          outId == (host->audioOutputUsed ? host->audioOutputDevice : std::string{}) &&
                          inId == (host->audioInputUsed ? host->audioInputDevice : std::string{}) &&
                          shownRate() == stream.sampleRate && shownFrames() == stream.bufferFrames &&
                          shownChoice(false).first == host->deviceOutputFirstChannel &&
                          (inId.empty() || shownChoice(true).first == host->deviceInputFirstChannel)};

  std::string text;
  if (showsRunning)
  {
    const bool hasIn{stream.inputChannels > 0 && stream.inputLatency > 0};
    const bool hasOut{stream.outputChannels > 0 && stream.outputLatency > 0};
    if (hasIn) text = "in " + formatMs(stream.inputLatency, stream.sampleRate) + " ms";
    if (hasOut)
      text += (text.empty() ? "" : ", ") + std::string{"out "} +
              formatMs(stream.outputLatency, stream.sampleRate) + " ms";
    if (!text.empty()) text = "Latency: " + text;
  }
  latencyNote.text = text;
}

void SettingsWindow::fillMidi()
{
  takesMidi = host->hasMIDIInput || host->hasClapNoteInput;
  sendsMidi = host->createsMidiOutput;

  // rows already there keep what they show; a saved port that is not plugged in is listed
  auto fill = [](Control &list, const std::vector<MidiPort> &ports,
                 const std::vector<std::string> &onIds, const std::vector<std::string> &absentNames,
                 bool allOn)
  {
    const auto previous{list.rows};
    list.rows.clear();
    auto add = [&](const std::string &reference, const std::string &name, bool connected, bool on)
    {
      Control::Row row{reference, name, on, on, connected};
      for (const auto &before : previous)
      {
        if (before.reference != reference) continue;
        row.on = before.on;
        row.wasOn = before.wasOn;
      }
      list.rows.push_back(row);
    };
    for (const auto &port : ports)
      add(port.id, port.name, true,
          allOn || std::find(onIds.begin(), onIds.end(), port.id) != onIds.end());
    if (!allOn)
    {
      for (const auto &name : absentNames) add(name, name, false, true);
    }
    list.scroll = std::min(list.scroll, std::max(0, static_cast<int>(list.rows.size()) - 1));
  };

  std::vector<std::string> onIds, absentNames;
  host->savedMidiSelection(true, onIds, absentNames);
  fill(midiIn, host->getMidiInputPorts(), onIds, absentNames, midiInAllOn);
  onIds.clear();
  absentNames.clear();
  host->savedMidiSelection(false, onIds, absentNames);
  fill(midiOut, host->getMidiOutputPorts(), onIds, absentNames, false);
}

void SettingsWindow::fillAppearance()
{
  const auto supported{supportedScreenAppearances()};
  appearance.items.clear();
  appearance.tags.clear();
  appearance.selected = -1;
  const std::pair<uint32_t, const char *> all[]{{CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO, "Auto"},
                                                {CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT, "Light"},
                                                {CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK, "Dark"}};
  for (const auto &[value, name] : all)
  {
    if (!(supported & (1u << value))) continue;
    if (value == host->screenAppearance) appearance.selected = static_cast<int>(appearance.items.size());
    appearance.items.push_back(name);
    appearance.tags.push_back(value);
  }
  appearance.visible = appearance.items.size() > 1 && !host->screenAppearanceFromPlugin;
}

// ---------------------------------------------------------------------------------------
// live apply

SettingsWindow::AudioSetup SettingsWindow::shownSetup() const
{
  AudioSetup setup;
  setup.running = true;
  if (const auto *out{shownDevice(false)})
  {
    setup.outputId = out->id;
    setup.useOutput = true;
  }
  if (const auto *in{shownDevice(true)})
  {
    setup.inputId = in->id;
    setup.useInput = true;
  }
  setup.rate = static_cast<int32_t>(shownRate());
  setup.frames = shownFrames();
  setup.outputChannels = shownChoice(false);
  setup.inputChannels = shownChoice(true);
  return setup;
}

SettingsWindow::AudioSetup SettingsWindow::runningSetup() const
{
  AudioSetup setup;
  setup.running = host->isAudioRunning();
  setup.inputId = host->audioInputDevice;
  setup.outputId = host->audioOutputDevice;
  setup.useInput = host->audioInputUsed;
  setup.useOutput = host->audioOutputUsed;
  setup.rate = host->currentSampleRate;
  setup.frames = host->currentBufferSize;
  setup.inputChannels = {host->deviceInputFirstChannel, host->deviceInputChannels, {}};
  setup.outputChannels = {host->deviceOutputFirstChannel, host->deviceOutputChannels, {}};
  return setup;
}

// whether the stream running now is this one: then opening it again would only drop out
bool SettingsWindow::runsAs(const AudioSetup &shown) const
{
  const auto now{runningSetup()};
  return !apiChanged && now.running && shown.rate == now.rate &&
         (shown.frames == 0 || shown.frames == now.frames) &&
         shown.useOutput == (host->currentOutputChannels > 0) &&
         (!shown.useOutput ||
          (shown.outputId == now.outputId && shown.outputChannels.first == now.outputChannels.first &&
           shown.outputChannels.count == now.outputChannels.count)) &&
         shown.useInput == (host->currentInputChannels > 0) &&
         (!shown.useInput ||
          (shown.inputId == now.inputId && shown.inputChannels.first == now.inputChannels.first &&
           shown.inputChannels.count == now.inputChannels.count));
}

// opens a stream on this setup; false, with the error already shown, if it does not open
bool SettingsWindow::openSetup(const AudioSetup &setup)
{
  if (setup.frames > 0) host->currentBufferSize = setup.frames;
  host->deviceInputFirstChannel = setup.inputChannels.first;
  host->deviceInputChannels = std::max<uint32_t>(setup.inputChannels.count, 1);
  host->deviceOutputFirstChannel = setup.outputChannels.first;
  host->deviceOutputChannels = std::max<uint32_t>(setup.outputChannels.count, 1);
  host->startAudioThreadOn(setup.inputId, host->deviceInputChannels, setup.useInput, setup.outputId,
                           host->deviceOutputChannels, setup.useOutput, setup.rate);
  return host->isAudioRunning();
}

// what the menus show, opened at once. a choice that does not open has its error shown
// (by the host), the stream before it opened again, and the menus back on what runs
bool SettingsWindow::openShownAudio()
{
  const auto shown{shownSetup()};
  if (!shown.useOutput || !okButton.enabled)
  {
    updateLatencyNote();
    return false;
  }
  if (runsAs(shown))
  {
    updateLatencyNote();
    return true;
  }

  const auto before{runningSetup()};
  if (openSetup(shown))
  {
    audioOpenedHere = true;
    apiChanged = false;
    // the size the device granted, where it is not the one asked for
    if (shown.frames != host->currentBufferSize) fillBufferSizes(host->currentBufferSize);
    updateLatencyNote();
    return true;
  }

  if (apiChanged)
  {
    // the other api's devices do not open: back to the api, and the devices, it had
    host->setAudioApi(apiWhenOpened);
    apiChanged = false;
    if (audioWhenOpened.running) openSetup(audioWhenOpened);
    fillApis();
  }
  else if (before.running)
  {
    openSetup(before);
  }
  showRunningSetup();
  return false;
}

// the menus on the stream that runs, or on the host's devices when none does
void SettingsWindow::showRunningSetup()
{
  fillDevices(host->audioOutputDevice, host->audioInputUsed ? host->audioInputDevice : std::string{});
  fillChannels(host->deviceOutputFirstChannel, host->deviceInputFirstChannel);
  fillRates(static_cast<uint32_t>(std::max(host->currentSampleRate, 0)));
  fillBufferSizes(host->currentBufferSize);
  updateInputNote();
}

void SettingsWindow::onApiChanged()
{
  if (api.selected < 0 || static_cast<size_t>(api.selected) >= apis.size()) return;
  const auto &id{apis[static_cast<size_t>(api.selected)].id};
  if (id == host->audioApiName) return;

  // another api's devices are listed only by switching to it, which stops the stream.
  // OK keeps it; Cancel goes back to the api, and the devices, the window opened with
  const bool hadInput{shownDevice(true) != nullptr};
  const auto before{runningSetup()};
  const auto previousApi{host->audioApiName};
  const auto name{apis[static_cast<size_t>(api.selected)].name};
  host->setAudioApi(id);
  if (host->audioApiName != id)
  {
    // an api with no server is replaced by one that has (createAudioBackend): not what
    // was chosen here. back to the api and the stream there were
    if (host->audioApiName != previousApi) host->setAudioApi(previousApi);
    if (before.running) openSetup(before);
    fillApis();
    showRunningSetup();
    showError(name + " is not running.");
    return;
  }
  apiChanged = true;

  // its default devices; the input stays off if it was, and if the default is a
  // microphone, which no default opens
  auto [defaultIn, defaultOut, defaultRate] = host->getDefaultAudioInOutSampleRate();
  const auto openable{host->defaultInputToOpen()};
  fillDevices(defaultOut, hadInput ? openable : std::string{});
  fillChannels(0, 0);
  fillRates(static_cast<uint32_t>(std::max(defaultRate, 0)));
  fillBufferSizes(shownFrames());
  updateInputNote();
  openShownAudio();
}

void SettingsWindow::onDeviceChanged()
{
  // the same channels on the other device, where it has them
  const auto outFirst{shownChoice(false).first}, inFirst{shownChoice(true).first};
  const auto r{shownRate()};
  const auto frames{shownFrames()};
  fillChannels(outFirst, inFirst);
  fillRates(r);
  fillBufferSizes(frames);
  updateInputNote();
  openShownAudio();
}

void SettingsWindow::onRateChanged()
{
  updateBufferTitles();
  openShownAudio();
}

void SettingsWindow::onDefaults()
{
  auto [defaultIn, defaultOut, defaultRate] = host->getDefaultAudioInOutSampleRate();
  // what a first run opens: never a microphone, an interface in its place where there is
  // one, otherwise "No Input", and the note says why
  errorNote.text.clear();
  fillDevices(defaultOut, host->defaultInputToOpen());
  fillChannels(0, 0);
  fillRates(static_cast<uint32_t>(std::max(defaultRate, 0)));
  fillBufferSizes(StandaloneSettings::defaultBufferSize);
  updateInputNote();
  openShownAudio();
  layout();
}

void SettingsWindow::onAppearanceChanged()
{
  // at once, through onScreenAppearanceChanged; saved by OK
  if (appearance.selected >= 0)
    host->setScreenAppearance(
        static_cast<uint32_t>(appearance.tags[static_cast<size_t>(appearance.selected)]), false);
}

void SettingsWindow::appearanceChanged()
{
  theme = Theme::make(display, host->screenAppearance == CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK);
  if (window) XSetWindowBackground(display, window, theme.background);
  fillAppearance();
  updateInputNote();
  layout();
}

void SettingsWindow::devicesChanged()
{
  const auto *out{shownDevice(false)};
  const auto *in{shownDevice(true)};
  const std::string outId{out ? out->id : std::string{}}, inId{in ? in->id : std::string{}};
  const auto r{shownRate()};
  const auto frames{shownFrames()};
  const auto outFirst{shownChoice(false).first}, inFirst{shownChoice(true).first};

  fillDevices(outId, inId);
  fillChannels(outFirst, inFirst);
  fillRates(r);
  fillBufferSizes(frames);
  updateInputNote();
  fillMidi();
  layout();
}

void SettingsWindow::tick()
{
  const auto before{latencyNote.text};
  updateLatencyNote();
  if (latencyNote.text != before) layout();
}

void SettingsWindow::showError(const std::string &message)
{
  errorNote.text = message;
  errorNote.color = theme.critical;
  errorNote.colored = true;
  layout();
}

void SettingsWindow::ok()
{
  if (!okButton.enabled) return;

  // every audio menu has opened what it shows already; not when the stream stopped since,
  // or a choice did not open. one that does not open now keeps the window up
  const auto shown{shownSetup()};
  if (!openShownAudio())
  {
    layout();
    return;
  }

  // what the user chose, not what was opened, which may be a fallback
  auto &settings{host->settings};
  host->setChosenDevices(shown.useOutput ? shown.outputId : std::string{},
                         shown.useInput ? shown.inputId : std::string{});
  settings.audioOutputUsed = shown.useOutput;
  settings.audioInputUsed = shown.useInput;
  if (shown.useOutput) settings.outputFirstChannel = shown.outputChannels.first + 1;
  if (shown.useInput) settings.inputFirstChannel = shown.inputChannels.first + 1;
  settings.sampleRate = shown.rate;

  // a port list is applied only when it was changed, so that an untouched input list
  // keeps "every port" if that is what it had
  auto changed = [](const Control &list)
  {
    return std::any_of(list.rows.begin(), list.rows.end(),
                       [](const auto &r) { return r.on != r.wasOn; });
  };
  auto ticked = [](const Control &list)
  {
    std::vector<std::string> references;
    for (const auto &r : list.rows)
    {
      if (r.on) references.push_back(r.reference);
    }
    return references;
  };
  if (takesMidi && changed(midiIn))
  {
    host->setChosenMidiPorts(true, ticked(midiIn));
    host->openMidiPorts(ticked(midiIn), false);
  }
  if (sendsMidi && changed(midiOut))
  {
    host->setChosenMidiPorts(false, ticked(midiOut));
    host->openMidiOutputPorts(ticked(midiOut));
  }

  // likewise, so that an application connected to a virtual port keeps its connection
  if (virtualIn.visible && virtualIn.checked != virtualInWasOn)
  {
    settings.midiVirtualInput = virtualIn.checked;
    host->setVirtualMidiInput(virtualIn.checked);
  }
  if (virtualOut.visible && virtualOut.checked != virtualOutWasOn)
  {
    settings.midiVirtualOutput = virtualOut.checked;
    host->setVirtualMidiOutput(virtualOut.checked);
  }

  // the window's own choice, never a plugin's
  if (!host->screenAppearanceFromPlugin) settings.screenAppearance = host->screenAppearance;

  host->captureAudioSettings();
  if (!host->saveStandaloneSettings())
    LOGINFO("[WARNING] Unable to save the audio settings; the next launch will not have them");

  closedWith = CLAP_WRAPPER_STANDALONE_CONFIGURATION_APPLIED;
  XUnmapWindow(display, window);
  XFlush(display);
}

// Cancel, Escape and the window manager's close all end here
void SettingsWindow::close()
{
  if (closedWith) return;
  if (popup) closeMenu(false);

  // an appearance tried and not confirmed goes back
  if (host->screenAppearance != appearanceWhenOpened)
    host->setScreenAppearance(appearanceWhenOpened, false);

  // likewise the audio the menus opened: what ran when the window opened runs again.
  // when nothing ran, the saved settings are tried as a launch tries them, quietly
  if (audioOpenedHere || apiChanged)
  {
    if (host->audioApiName != apiWhenOpened) host->setAudioApi(apiWhenOpened);
    if (audioWhenOpened.running)
    {
      openSetup(audioWhenOpened);
    }
    else
    {
      host->applyAudioSettings();
      StandaloneHost::SilenceAudioErrors silence(host);
      host->startAudioThreadOn(host->audioInputDevice, host->deviceInputChannels,
                               host->audioInputUsed && host->numAudioInputs > 0, host->audioOutputDevice,
                               host->deviceOutputChannels,
                               host->audioOutputUsed && host->numAudioOutputs > 0,
                               host->currentSampleRate);
    }
  }

  closedWith = CLAP_WRAPPER_STANDALONE_CONFIGURATION_CANCELLED;
  XUnmapWindow(display, window);
  XFlush(display);
}
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
