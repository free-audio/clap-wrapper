#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <clap/clap.h>

namespace freeaudio::clap_wrapper::standalone
{
/*
 * What the standalone's window shows before the plugin's gui exists: the size the gui is
 * going to ask for, its background colour, and an image for the middle of the window.
 *
 * Making the plugin and its gui takes a moment, and only the gui knows its size. With these,
 * the window comes up at once at that size, in the gui's colour, with the image in its
 * middle, and the plugin's view then takes its place without the window moving or changing
 * colour. Without them the window is shown once the gui has its size.
 *
 * They come from the build (STANDALONE_WINDOW_SIZE, STANDALONE_BACKGROUND_COLOR,
 * STANDALONE_SPLASH_IMAGE, STANDALONE_SPLASH_IMAGE_SCALE in make_clapfirst_plugins), and
 * the plugin's entry can answer each of them itself, per plugin
 * (clap_wrapper_plugin_factory_standalone_window in clapwrapper/standalone_features.h).
 * The entry's answer wins.
 */
struct WindowHints
{
  // the content size to open the window at when it has no place of its own from an earlier
  // run, in the units clap_plugin_gui sizes are in at a scale of 1 (points on macOS, pixels
  // at 96 dpi on Windows, which the window scales by its dpi); 0 x 0 when there is none
  uint32_t width{0}, height{0};

  // 0xRRGGBB, under the image and around the plugin's view
  bool hasBackground{false};
  uint32_t background{0};

  // a png, drawn in the middle of the window until the plugin's view is in place; nullptr
  // when there is none. scale is how many of its pixels make one unit of the size: an image
  // made for a 2x screen has 2
  const unsigned char *splashPng{nullptr};
  size_t splashPngSize{0};
  uint32_t splashScale{1};

  // text on the splash, from the entry only: lines separated by '\n', each centred on the
  // image's middle, the first one's top edge textTop below the image's top edge (or the
  // window's middle, without an image), in the units of the size; empty for none
  std::string text;
  uint32_t textColor{0xffffff};
  uint32_t textSize{12};
  int32_t textTop{0};

  // a clap_wrapper_standalone_appearance the entry named, which mainPrepare sets as the
  // plugin's own; from the entry only
  bool hasAppearance{false};
  uint32_t appearance{0};

  bool hasSize() const
  {
    return width > 0 && height > 0;
  }
};

// the build's hints, and once readPluginWindowHints has run, the entry's over them
const WindowHints &windowHints();

// asks the plugin's entry for its hints for pluginId, if it has the factory, and puts each
// one it gives over the build's. mainPrepare calls it, before the plugin exists
void readPluginWindowHints(const clap_plugin_entry *entry, const void *pluginFactory,
                           const std::string &pluginId);
}  // namespace freeaudio::clap_wrapper::standalone
