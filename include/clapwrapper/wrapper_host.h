#ifndef CLAPWRAPPER_WRAPPER_HOST_H
#define CLAPWRAPPER_WRAPPER_HOST_H

#include "clap/private/macros.h"
#include "clap/host.h"

// CLAP_ABI was introduced in CLAP 1.1.2, for older versions we make it transparent
#ifndef CLAP_ABI
#define CLAP_ABI
#endif

/*
  clap_wrapper_host_information

  a host extension every clap-wrapper host provides, so a plugin can tell it is wrapped
  and in what. a null result from get_extension means not wrapped.

    auto *cwh = (const clap_wrapper_host_information_t *)host->get_extension(
        host, CLAP_WRAPPER_HOST_INFORMATION);
    if (cwh && strcmp(cwh->get_wrapper_flavor(host), CLAP_WRAPPER_HOST_FLAVOR_AUV2) == 0)
      ...
*/

#ifdef __cplusplus
extern "C"
{
#endif

  static const CLAP_CONSTEXPR char CLAP_WRAPPER_HOST_INFORMATION[] = "clap-wrapper.host-information/0";

  // compare with strcmp; the pointers are not guaranteed to be these arrays
  static const CLAP_CONSTEXPR char CLAP_WRAPPER_HOST_FLAVOR_AUV2[] = "auv2";
  static const CLAP_CONSTEXPR char CLAP_WRAPPER_HOST_FLAVOR_AUV3[] = "auv3";
  static const CLAP_CONSTEXPR char CLAP_WRAPPER_HOST_FLAVOR_AAX[] = "aax";
  static const CLAP_CONSTEXPR char CLAP_WRAPPER_HOST_FLAVOR_VST3[] = "vst3";
  static const CLAP_CONSTEXPR char CLAP_WRAPPER_HOST_FLAVOR_STANDALONE[] = "standalone";

  typedef struct clap_wrapper_host_information
  {
    // one of CLAP_WRAPPER_HOST_FLAVOR_*, valid for the life of the host
    // [thread-safe]
    const char *(CLAP_ABI *get_wrapper_flavor)(const clap_host_t *host);

    // the real host's name without host->name's wrapper suffix, e.g. "REAPER"
    // null when unknowable
    // [thread-safe]
    const char *(CLAP_ABI *get_underlying_host_name)(const clap_host_t *host);
  } clap_wrapper_host_information_t;

#ifdef __cplusplus
}
#endif

#endif
