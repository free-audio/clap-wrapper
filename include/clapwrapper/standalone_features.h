#ifndef CLAPWRAPPER_STANDALONE_FEATURES_H
#define CLAPWRAPPER_STANDALONE_FEATURES_H

#include "clap/private/macros.h"
#include "clap/host.h"
#include "clap/string-sizes.h"

// CLAP_ABI was introduced in CLAP 1.1.2, for older versions we make it transparent
#ifndef CLAP_ABI
#define CLAP_ABI
#endif

/*
  clap_wrapper_standalone_features  (DRAFT)

  a host extension the clap-wrapper standalone provides so a plugin can drive the
  standalone's audio and midi setup from its own ui. a null result from get_extension
  means the plugin is not running in a clap-wrapper standalone.

    auto *sf = (const clap_wrapper_standalone_features_t *)host->get_extension(
        host, CLAP_WRAPPER_STANDALONE_FEATURES);
    if (sf && sf->supports_audio_midi_configuration_screen(host))
      sf->show_audio_midi_configuration_screen(host);

  every function is [main-thread]. every feature has a supports_ query, and a call to
  an unsupported feature returns false/0 rather than misbehaving, so a plugin can
  call these unconditionally once it has the extension.

  devices and midi ports are called endpoints here, to avoid colliding with the
  plugin's own audio-ports and note-ports.

  identity and persistence: audio api ids and endpoint ids are names, never enumeration
  indices. rtaudio and rtmidi device ids are reshuffled on every boot and on every api
  switch, so a saved index opens whatever lands in that slot next time; a saved name
  either resolves to the same device or falls back to the system default. a plugin may
  keep ids in its own settings and hand them back in a later session.

  startup: the standalone creates and init()s the plugin before it opens any audio or
  midi. a request_ made before then (from init(), or from state load) is what the first
  open uses, resolved by name at that point, so a plugin which keeps its own settings
  never sees the devices open on the defaults and then bounce.

  changes made through this extension persist exactly as if the user had made them
  in the standalone's own settings screen.
*/

#ifdef __cplusplus
extern "C"
{
#endif

  static const CLAP_CONSTEXPR char CLAP_WRAPPER_STANDALONE_FEATURES[] =
      "clap-wrapper.standalone-features.draft/1";

  enum clap_wrapper_standalone_endpoint_kind
  {
    CLAP_WRAPPER_STANDALONE_AUDIO_INPUT = 0,
    CLAP_WRAPPER_STANDALONE_AUDIO_OUTPUT = 1,
    CLAP_WRAPPER_STANDALONE_MIDI_INPUT = 2,
    CLAP_WRAPPER_STANDALONE_MIDI_OUTPUT = 3,
  };

  enum clap_wrapper_standalone_flags
  {
    // the system default for its kind
    CLAP_WRAPPER_STANDALONE_IS_DEFAULT = 1 << 0,

    // currently open and in use by the standalone
    CLAP_WRAPPER_STANDALONE_IS_BOUND = 1 << 1,
  };

  typedef struct clap_wrapper_standalone_audio_api
  {
    // rtaudio's api name, e.g. "core", "alsa", "pulse", "jack", "asio", "wasapi", "ds"
    char id[CLAP_NAME_SIZE];

    // for display, e.g. "Windows WASAPI"
    char name[CLAP_NAME_SIZE];

    // clap_wrapper_standalone_flags
    uint32_t flags;
  } clap_wrapper_standalone_audio_api_t;

  typedef struct clap_wrapper_standalone_endpoint
  {
    // the device or port name; hand it back to the request_ functions
    char id[CLAP_PATH_SIZE];

    // for display; may equal id
    char name[CLAP_NAME_SIZE];

    // audio endpoints only, 0 for midi
    uint32_t channel_count;

    // clap_wrapper_standalone_flags
    uint32_t flags;
  } clap_wrapper_standalone_endpoint_t;

  typedef struct clap_wrapper_standalone_audio_config
  {
    // nullptr keeps the current api, "" means the platform default. switching api
    // resolves the device ids below against the new api's devices
    const char *api_id;

    // nullptr means no device on that side, "" means follow the system default.
    // a plugin with no audio inputs never opens an input, whatever is asked here
    const char *input_id;
    const char *output_id;

    // 0 means the device's preferred rate
    uint32_t sample_rate;

    // frames; 0 means the standalone's default
    uint32_t buffer_size;
  } clap_wrapper_standalone_audio_config_t;

  typedef struct clap_wrapper_standalone_audio_status
  {
    // true from a request_audio_config until the standalone has applied it
    bool request_pending;

    // false if no stream is open, e.g. no output device attached or the open failed
    bool running;

    // the api the stream is on, as in clap_wrapper_standalone_audio_api.id
    char api_id[CLAP_NAME_SIZE];

    // what the stream actually opened at, which may differ from what was requested.
    // 0 when not running
    uint32_t sample_rate;
    uint32_t buffer_size;
    uint32_t input_channels;
    uint32_t output_channels;

    // the error from the most recent open, "" if it succeeded. survives until the
    // next open attempt, so a plugin polling after a request sees why it failed
    char last_error[CLAP_PATH_SIZE];
  } clap_wrapper_standalone_audio_status_t;

  enum clap_wrapper_standalone_reset_flags
  {
    // api, devices, rate, buffer size and midi ports back to the system defaults
    CLAP_WRAPPER_STANDALONE_RESET_AUDIO_MIDI = 1 << 0,

    // forget the saved window position and size
    CLAP_WRAPPER_STANDALONE_RESET_WINDOW = 1 << 1,

    // load the state the plugin had when the standalone launched, before any
    // auto-saved session was restored, and stop that session being restored again
    CLAP_WRAPPER_STANDALONE_RESET_PLUGIN_STATE = 1 << 2,
  };

  typedef struct clap_wrapper_standalone_features
  {
    /* -- the standalone's own settings screen -- */

    // [main-thread]
    bool(CLAP_ABI *supports_audio_midi_configuration_screen)(const clap_host_t *host);

    // opens the standalone's audio/midi settings window, or brings it to the front.
    // non-modal: returns once the window is up, not when the user closes it.
    // returns false if unsupported or the window could not be shown
    // [main-thread]
    bool(CLAP_ABI *show_audio_midi_configuration_screen)(const clap_host_t *host);

    /* -- audio apis (coreaudio, alsa, pulse, jack, asio, wasapi, ...) -- */

    // the apis compiled into this standalone. IS_BOUND marks the current one.
    // devices are listed for the current api only; to browse another, request it
    // and list again once get_audio_status shows it applied
    // [main-thread]
    uint32_t(CLAP_ABI *count_audio_apis)(const clap_host_t *host);

    // [main-thread]
    bool(CLAP_ABI *get_audio_api)(const clap_host_t *host, uint32_t index,
                                  clap_wrapper_standalone_audio_api_t *api);

    /* -- endpoints -- */

    // kind is a clap_wrapper_standalone_endpoint_kind
    // [main-thread]
    bool(CLAP_ABI *supports_listing)(const clap_host_t *host, uint32_t kind);

    // snapshots the endpoints of kind and returns how many there are. get_endpoint
    // answers from the snapshot until the next count_endpoints for that kind. the
    // standalone may answer from a cached enumeration (asio probes every driver to
    // enumerate), so a device plugged in since may not show until it rescans
    // [main-thread]
    uint32_t(CLAP_ABI *count_endpoints)(const clap_host_t *host, uint32_t kind);

    // returns false if index is out of range of the last count_endpoints snapshot
    // [main-thread]
    bool(CLAP_ABI *get_endpoint)(const clap_host_t *host, uint32_t kind, uint32_t index,
                                 clap_wrapper_standalone_endpoint_t *endpoint);

    /* -- stream parameters -- */

    // snapshots the rates a stream on these devices could run at, ids as in
    // clap_wrapper_standalone_audio_config. that is the output device's rates,
    // narrowed by the input device's only when the plugin has audio inputs and
    // input_id is not nullptr: an instrument never opens the input, so it never
    // narrows the list
    // [main-thread]
    uint32_t(CLAP_ABI *count_sample_rates)(const clap_host_t *host, const char *output_id,
                                           const char *input_id);

    // 0 if index is out of range of the last count_sample_rates snapshot
    // [main-thread]
    uint32_t(CLAP_ABI *get_sample_rate)(const clap_host_t *host, uint32_t index);

    // the buffer sizes the standalone offers. rtaudio cannot ask a device what it
    // will accept, so this is a fixed list rather than a device's answer; the
    // standalone asks for the size and get_audio_status reports what it got
    // [main-thread]
    uint32_t(CLAP_ABI *count_buffer_sizes)(const clap_host_t *host);

    // 0 if index is out of range
    // [main-thread]
    uint32_t(CLAP_ABI *get_buffer_size)(const clap_host_t *host, uint32_t index);

    /* -- status -- */

    // returns false if unsupported
    // [main-thread]
    bool(CLAP_ABI *get_audio_status)(const clap_host_t *host,
                                     clap_wrapper_standalone_audio_status_t *status);

    /* -- rebinding -- */

    // [main-thread]
    bool(CLAP_ABI *supports_binding)(const clap_host_t *host, uint32_t kind);

    // reopens the audio stream with this configuration. never applied re-entrantly:
    // the standalone deactivates and reactivates the plugin from its own main loop
    // after this returns, and request_pending stays true until it has. an id which
    // does not resolve falls back to the system default. returns false if
    // unsupported or config is malformed
    // [main-thread]
    bool(CLAP_ABI *request_audio_config)(const clap_host_t *host,
                                         const clap_wrapper_standalone_audio_config_t *config);

    // replaces the open midi endpoints of kind with exactly these ids. count 0 opens
    // none. bind_all ignores ids and opens every endpoint present now, which is what
    // a fresh standalone does. ids not present now are remembered and logged, not opened.
    // returns false if unsupported or kind is not a midi kind
    // [main-thread]
    bool(CLAP_ABI *request_midi_bindings)(const clap_host_t *host, uint32_t kind, const char *const *ids,
                                          uint32_t count, bool bind_all);

    /* -- reset -- */

    // the clap_wrapper_standalone_reset_flags this standalone honours; 0 for none
    // [main-thread]
    uint32_t(CLAP_ABI *supported_reset_flags)(const clap_host_t *host);

    // unsupported flags are ignored. like request_audio_config, audio changes are
    // applied after this returns. returns false if no requested flag was supported
    // [main-thread]
    bool(CLAP_ABI *reset_standalone_state)(const clap_host_t *host, uint32_t flags);
  } clap_wrapper_standalone_features_t;

#ifdef __cplusplus
}
#endif

#endif
