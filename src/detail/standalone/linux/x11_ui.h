#pragma once

/*
 * What the Linux standalone draws itself with, on plain Xlib: a font, the colours of a
 * light and a dark theme, and a few drawing helpers. No toolkit and no new library: the
 * strip above the plugin and the settings window are drawn with these.
 *
 * Text is UTF-8 throughout. It is drawn with a core font through XDrawString16, a
 * Unicode (iso10646-1) one where the server has one, so that device names in any script
 * show where the font has their glyphs, without touching the process's locale.
 */

#include <X11/Xlib.h>

#include <string>
#include <vector>

namespace freeaudio::clap_wrapper::standalone::linux_standalone
{
struct Rect
{
  int x{0}, y{0}, w{0}, h{0};
  bool contains(int px, int py) const
  {
    return px >= x && py >= y && px < x + w && py < y + h;
  }
};

class UiFont
{
 public:
  // the first of a list of fonts the server has: proportional Unicode ones, then fixed,
  // then Latin-1 ones. false if not even "fixed" is there
  bool load(Display *display);
  void unload(Display *display);

  int ascent() const
  {
    return fs ? fs->ascent : 10;
  }
  int height() const
  {
    return fs ? fs->ascent + fs->descent : 13;
  }
  int width(const std::string &text) const;

  // the text, shortened with "…" at the end to fit
  std::string fit(const std::string &text, int maxWidth) const;

  // the text broken into lines no wider than this, at spaces
  std::vector<std::string> wrap(const std::string &text, int maxWidth) const;

  void draw(Display *display, Drawable drawable, GC gc, int x, int baseline,
            const std::string &text) const;

 private:
  std::vector<XChar2b> encode(const std::string &text) const;

  XFontStruct *fs{nullptr};
  bool unicode{false};
};

struct Theme
{
  unsigned long background{0}, strip{0}, text{0}, secondary{0}, disabled{0};
  unsigned long field{0}, fieldBorder{0}, hover{0}, accent{0}, accentText{0};
  unsigned long critical{0}, caution{0}, rule{0};

  // dark for CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK; light for light and auto, which
  // has no system setting to follow on X11
  static Theme make(Display *display, bool dark);
};

// drawing into one drawable with one GC
struct Painter
{
  Display *display{nullptr};
  Drawable drawable{0};
  GC gc{nullptr};
  const UiFont *font{nullptr};

  void fill(const Rect &r, unsigned long color) const;
  void frame(const Rect &r, unsigned long color) const;
  void line(int x1, int y1, int x2, int y2, unsigned long color) const;
  // text with its top at y
  void text(int x, int y, const std::string &utf8, unsigned long color) const;
  // text centred vertically in r, from its left edge plus pad
  void textIn(const Rect &r, int pad, const std::string &utf8, unsigned long color) const;
  // a small downward triangle, the mark of a menu
  void chevron(int cx, int cy, unsigned long color) const;
  // a tick inside a box
  void tick(const Rect &box, unsigned long color) const;
};

}  // namespace freeaudio::clap_wrapper::standalone::linux_standalone
