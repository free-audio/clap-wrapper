/*
 * distortion_clap_entry
 *
 * This uses extern versions of the entry methods from a static library to create
 * a working exported C linkage clap entry point
 */

#include <clap/clap.h>
#include <cstring>

#include "distortion_clap_entry.h"

extern "C"
{
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"  // other peoples errors are outside my scope
#endif

  const CLAP_EXPORT struct clap_plugin_entry clap_entry = {CLAP_VERSION, dist_entry_init,
                                                           dist_entry_deinit, dist_entry_get_factory};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}