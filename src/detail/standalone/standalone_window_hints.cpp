#include "standalone_window_hints.h"
#include "standalone_details.h"

#include "clapwrapper/standalone_features.h"

#include <cstring>

namespace freeaudio::clap_wrapper::standalone
{
#ifdef CLAP_WRAPPER_STANDALONE_SPLASH_IMAGE
// the png's bytes, in a source the build writes from SPLASH_IMAGE (wrap_standalone.cmake)
extern const unsigned char splashImagePng[];
extern const size_t splashImagePngSize;
#endif

namespace
{
WindowHints builtHints()
{
  WindowHints hints;
#if defined(CLAP_WRAPPER_STANDALONE_WINDOW_WIDTH) && defined(CLAP_WRAPPER_STANDALONE_WINDOW_HEIGHT)
  hints.width = CLAP_WRAPPER_STANDALONE_WINDOW_WIDTH;
  hints.height = CLAP_WRAPPER_STANDALONE_WINDOW_HEIGHT;
#endif
#ifdef CLAP_WRAPPER_STANDALONE_BACKGROUND_COLOR
  hints.hasBackground = true;
  hints.background = CLAP_WRAPPER_STANDALONE_BACKGROUND_COLOR;
#endif
#ifdef CLAP_WRAPPER_STANDALONE_SPLASH_IMAGE
  hints.splashPng = splashImagePng;
  hints.splashPngSize = splashImagePngSize;
  hints.splashScale = CLAP_WRAPPER_STANDALONE_SPLASH_SCALE;
#endif
  return hints;
}

WindowHints &hintsInUse()
{
  static WindowHints hints{builtHints()};
  return hints;
}

bool isPng(const uint8_t *bytes, uint32_t size)
{
  static const uint8_t signature[8]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  return bytes && size > sizeof(signature) && std::memcmp(bytes, signature, sizeof(signature)) == 0;
}
}  // namespace

const WindowHints &windowHints()
{
  return hintsInUse();
}

void readPluginWindowHints(const clap_plugin_entry *entry, const void *pluginFactory,
                           const std::string &pluginId)
{
  if (!entry || !entry->get_factory || pluginId.empty()) return;

  auto *factory{static_cast<const clap_wrapper_plugin_factory_standalone_window_t *>(
      entry->get_factory(CLAP_WRAPPER_PLUGIN_FACTORY_STANDALONE_WINDOW))};
  // some entries answer every id with their plugin factory
  if (!factory || factory == pluginFactory || !factory->get_window_hints) return;

  clap_wrapper_standalone_window_hints_t given{};
  if (!factory->get_window_hints(factory, pluginId.c_str(), &given)) return;

  auto &hints{hintsInUse()};

  if (given.flags & CLAP_WRAPPER_STANDALONE_WINDOW_HINT_SIZE)
  {
    if (given.width > 0 && given.height > 0 && given.width <= 16384 && given.height <= 16384)
    {
      hints.width = given.width;
      hints.height = given.height;
    }
    else
    {
      LOGINFO("[WARNING] The plugin's window size hint {} x {} is out of range; ignored", given.width,
              given.height);
    }
  }

  if (given.flags & CLAP_WRAPPER_STANDALONE_WINDOW_HINT_BACKGROUND)
  {
    hints.hasBackground = true;
    hints.background = given.background & 0xffffff;
  }

  if (given.flags & CLAP_WRAPPER_STANDALONE_WINDOW_HINT_SPLASH_IMAGE)
  {
    if (isPng(given.splash_png, given.splash_png_size) && given.splash_scale > 0)
    {
      hints.splashPng = given.splash_png;
      hints.splashPngSize = given.splash_png_size;
      hints.splashScale = given.splash_scale;
    }
    else
    {
      LOGINFO("[WARNING] The plugin's splash image is not a png, or has no scale; ignored");
    }
  }

  if ((given.flags & CLAP_WRAPPER_STANDALONE_WINDOW_HINT_SPLASH_TEXT) && given.splash_text)
  {
    hints.text = given.splash_text;
    hints.textColor = given.splash_text_color & 0xffffff;
    hints.textSize =
        given.splash_text_size > 0 && given.splash_text_size <= 512 ? given.splash_text_size : 12;
    hints.textTop = given.splash_text_top;
  }

  if (given.flags & CLAP_WRAPPER_STANDALONE_WINDOW_HINT_APPEARANCE)
  {
    hints.hasAppearance = true;
    hints.appearance = given.appearance;
  }
}
}  // namespace freeaudio::clap_wrapper::standalone
