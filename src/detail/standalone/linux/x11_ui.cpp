#include "x11_ui.h"

#include <algorithm>
#include <cstdint>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
namespace
{
// the code points of UTF-8 text; a broken sequence is U+FFFD
std::vector<uint32_t> decode(const std::string &text)
{
  std::vector<uint32_t> res;
  for (size_t i = 0; i < text.size();)
  {
    const auto c{static_cast<unsigned char>(text[i])};
    uint32_t cp{0};
    size_t extra{0};
    if (c < 0x80)
      cp = c;
    else if ((c & 0xE0) == 0xC0)
    {
      cp = c & 0x1F;
      extra = 1;
    }
    else if ((c & 0xF0) == 0xE0)
    {
      cp = c & 0x0F;
      extra = 2;
    }
    else if ((c & 0xF8) == 0xF0)
    {
      cp = c & 0x07;
      extra = 3;
    }
    else
    {
      res.push_back(0xFFFD);
      ++i;
      continue;
    }
    if (i + extra >= text.size() + (extra ? 0 : 1) && extra)
    {
      res.push_back(0xFFFD);
      break;
    }
    bool ok{true};
    for (size_t k = 1; k <= extra; ++k)
    {
      const auto b{static_cast<unsigned char>(text[i + k])};
      if ((b & 0xC0) != 0x80)
      {
        ok = false;
        break;
      }
      cp = (cp << 6) | (b & 0x3F);
    }
    res.push_back(ok ? cp : 0xFFFD);
    i += ok ? extra + 1 : 1;
  }
  return res;
}

// one code point back into UTF-8
void append(std::string &into, uint32_t cp)
{
  if (cp < 0x80)
    into += static_cast<char>(cp);
  else if (cp < 0x800)
  {
    into += static_cast<char>(0xC0 | (cp >> 6));
    into += static_cast<char>(0x80 | (cp & 0x3F));
  }
  else if (cp < 0x10000)
  {
    into += static_cast<char>(0xE0 | (cp >> 12));
    into += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    into += static_cast<char>(0x80 | (cp & 0x3F));
  }
  else
  {
    into += static_cast<char>(0xF0 | (cp >> 18));
    into += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    into += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    into += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

unsigned long color(Display *display, uint32_t rgb)
{
  XColor c{};
  c.red = static_cast<unsigned short>(((rgb >> 16) & 0xFF) * 257);
  c.green = static_cast<unsigned short>(((rgb >> 8) & 0xFF) * 257);
  c.blue = static_cast<unsigned short>((rgb & 0xFF) * 257);
  c.flags = DoRed | DoGreen | DoBlue;
  const auto screen{DefaultScreen(display)};
  if (!XAllocColor(display, DefaultColormap(display, screen), &c))
    return (rgb & 0xFFFFFF) > 0x808080 ? WhitePixel(display, screen) : BlackPixel(display, screen);
  return c.pixel;
}
}  // namespace

bool UiFont::load(Display *display)
{
  static const char *const candidates[]{
      "-*-dejavu sans-medium-r-normal--13-*-*-*-p-*-iso10646-1",
      "-*-liberation sans-medium-r-normal--13-*-*-*-p-*-iso10646-1",
      "-*-helvetica-medium-r-normal--12-*-*-*-p-*-iso10646-1",
      "-misc-fixed-medium-r-semicondensed--13-*-*-*-c-*-iso10646-1",
      "-misc-fixed-medium-r-normal--13-*-*-*-c-*-iso10646-1",
      "-*-helvetica-medium-r-normal--12-*-*-*-p-*-iso8859-1",
      "fixed",
  };
  for (const auto *name : candidates)
  {
    fs = XLoadQueryFont(display, name);
    if (!fs) continue;
    // a font with more than one byte's characters is a Unicode one
    unicode = fs->max_byte1 > 0;
    return true;
  }
  return false;
}

void UiFont::unload(Display *display)
{
  if (fs) XFreeFont(display, fs);
  fs = nullptr;
}

std::vector<XChar2b> UiFont::encode(const std::string &text) const
{
  std::vector<XChar2b> res;
  for (auto cp : decode(text))
  {
    // what the font cannot have: a question mark
    if (cp > 0xFFFF || (!unicode && cp > 0xFF)) cp = '?';
    XChar2b c;
    c.byte1 = static_cast<unsigned char>(cp >> 8);
    c.byte2 = static_cast<unsigned char>(cp & 0xFF);
    res.push_back(c);
  }
  return res;
}

int UiFont::width(const std::string &text) const
{
  if (!fs || text.empty()) return 0;
  auto chars{encode(text)};
  return XTextWidth16(fs, chars.data(), static_cast<int>(chars.size()));
}

std::string UiFont::fit(const std::string &text, int maxWidth) const
{
  if (width(text) <= maxWidth) return text;
  const std::string ellipsis{"\xE2\x80\xA6"};
  auto cps{decode(text)};
  while (!cps.empty())
  {
    cps.pop_back();
    std::string candidate;
    for (auto cp : cps) append(candidate, cp);
    candidate += ellipsis;
    if (width(candidate) <= maxWidth) return candidate;
  }
  return ellipsis;
}

std::vector<std::string> UiFont::wrap(const std::string &text, int maxWidth) const
{
  std::vector<std::string> lines;
  std::string line;
  size_t at{0};
  while (at < text.size())
  {
    auto space{text.find(' ', at)};
    if (space == std::string::npos) space = text.size();
    const auto word{text.substr(at, space - at)};
    const auto candidate{line.empty() ? word : line + " " + word};
    if (!line.empty() && width(candidate) > maxWidth)
    {
      lines.push_back(line);
      line = word;
    }
    else
    {
      line = candidate;
    }
    at = space + 1;
  }
  if (!line.empty()) lines.push_back(line);
  return lines;
}

void UiFont::draw(Display *display, Drawable drawable, GC gc, int x, int baseline,
                  const std::string &text) const
{
  if (!fs || text.empty()) return;
  XSetFont(display, gc, fs->fid);
  auto chars{encode(text)};
  XDrawString16(display, drawable, gc, x, baseline, chars.data(), static_cast<int>(chars.size()));
}

Theme Theme::make(Display *display, bool dark)
{
  Theme t;
  if (dark)
  {
    t.background = color(display, 0x2B2B2B);
    t.strip = color(display, 0x232323);
    t.text = color(display, 0xE8E8E8);
    t.secondary = color(display, 0xA8A8A8);
    t.disabled = color(display, 0x707070);
    t.field = color(display, 0x3A3A3A);
    t.fieldBorder = color(display, 0x5A5A5A);
    t.hover = color(display, 0x474747);
    t.accent = color(display, 0x3D7DD8);
    t.accentText = color(display, 0xFFFFFF);
    t.critical = color(display, 0xFF6B6B);
    t.caution = color(display, 0xF0A030);
    t.rule = color(display, 0x444444);
  }
  else
  {
    t.background = color(display, 0xF2F2F2);
    t.strip = color(display, 0xE4E4E4);
    t.text = color(display, 0x1E1E1E);
    t.secondary = color(display, 0x666666);
    t.disabled = color(display, 0xA0A0A0);
    t.field = color(display, 0xFFFFFF);
    t.fieldBorder = color(display, 0xB4B4B4);
    t.hover = color(display, 0xE8EEF8);
    t.accent = color(display, 0x2F6FCC);
    t.accentText = color(display, 0xFFFFFF);
    t.critical = color(display, 0xC62828);
    t.caution = color(display, 0xB86E00);
    t.rule = color(display, 0xD0D0D0);
  }
  return t;
}

void Painter::fill(const Rect &r, unsigned long c) const
{
  XSetForeground(display, gc, c);
  XFillRectangle(display, drawable, gc, r.x, r.y, static_cast<unsigned>(std::max(r.w, 0)),
                 static_cast<unsigned>(std::max(r.h, 0)));
}

void Painter::frame(const Rect &r, unsigned long c) const
{
  XSetForeground(display, gc, c);
  XDrawRectangle(display, drawable, gc, r.x, r.y, static_cast<unsigned>(std::max(r.w - 1, 0)),
                 static_cast<unsigned>(std::max(r.h - 1, 0)));
}

void Painter::line(int x1, int y1, int x2, int y2, unsigned long c) const
{
  XSetForeground(display, gc, c);
  XDrawLine(display, drawable, gc, x1, y1, x2, y2);
}

void Painter::text(int x, int y, const std::string &utf8, unsigned long c) const
{
  XSetForeground(display, gc, c);
  font->draw(display, drawable, gc, x, y + font->ascent(), utf8);
}

void Painter::textIn(const Rect &r, int pad, const std::string &utf8, unsigned long c) const
{
  text(r.x + pad, r.y + (r.h - font->height()) / 2, font->fit(utf8, r.w - 2 * pad), c);
}

void Painter::chevron(int cx, int cy, unsigned long c) const
{
  XSetForeground(display, gc, c);
  XPoint points[3]{{static_cast<short>(cx - 4), static_cast<short>(cy - 2)},
                   {static_cast<short>(cx + 4), static_cast<short>(cy - 2)},
                   {static_cast<short>(cx), static_cast<short>(cy + 3)}};
  XFillPolygon(display, drawable, gc, points, 3, Convex, CoordModeOrigin);
}

void Painter::tick(const Rect &box, unsigned long c) const
{
  XSetForeground(display, gc, c);
  XSetLineAttributes(display, gc, 2, LineSolid, CapRound, JoinRound);
  XPoint points[3]{{static_cast<short>(box.x + 3), static_cast<short>(box.y + box.h / 2)},
                   {static_cast<short>(box.x + box.w / 2 - 1), static_cast<short>(box.y + box.h - 4)},
                   {static_cast<short>(box.x + box.w - 3), static_cast<short>(box.y + 3)}};
  XDrawLines(display, drawable, gc, points, 3, CoordModeOrigin);
  XSetLineAttributes(display, gc, 1, LineSolid, CapButt, JoinMiter);
}
}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
