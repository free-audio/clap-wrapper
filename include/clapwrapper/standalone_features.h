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

  identity and persistence: audio api ids and endpoint ids are opaque strings, never
  enumeration indices. take them from the listing and hand them back unchanged; do not
  build or parse them. today they are the device or port names, and a platform may move
  to a stable identifier of its own (coreaudio's device uid, say) without a new draft, so
  the standalone keeps accepting the names an earlier session stored. a saved id either
  resolves to the same device or falls back to the system default. a plugin may keep ids
  in its own settings and hand them back in a later session.

  startup: the standalone creates and init()s the plugin before it opens any audio or
  midi. a request_ made before then (from init(), or from state load) is what the first
  open uses, resolved by name at that point, so a plugin which keeps its own settings
  never sees the devices open on the defaults and then bounce.

  changes made through this extension persist exactly as if the user had made them
  in the standalone's own settings screen.

  the plugin's state: by default the standalone saves it when it quits and loads it back
  at the next launch, over the state the plugin has after init(). a plugin that keeps
  its state its own way, or wants every launch to start fresh, says so with
  set_plugin_state_persistence, from init() for the launch's own restore to follow it.

  the plugin side: a plugin which returns clap_wrapper_plugin_standalone_features from
  get_extension under the same id hears when the user is done with that screen, so it
  can, say, unmute an input it muted before showing it. a standalone asks for it once,
  after init().

  the settings folder: the standalone keeps its files for a plugin - the audio and midi
  setup, the saved state - in a folder named after the plugin id, under a base folder of
  its own (clap-wrapper-standalone in the platform's settings directory). a plugin can
  name that base folder from its entry, before any instance exists: see
  clap_wrapper_plugin_factory_standalone_settings at the end of this file.

  draft/5, from draft/4:
  - the plugin's state across launches: whether the standalone restores the last
    session's, set by the plugin (*_plugin_state_persistence)
  - the settings folder, named by the plugin's entry (a factory of its own, at the end)
  new functions are at the end of the struct. draft/4 is no longer answered.

  draft/4, from draft/3:
  - the configuration screen's appearance: light, dark, or the system's, set by the plugin
  - channel selection: which device channels feed and take the plugin's main ports
    (audio_config/audio_status gain *_first_channel, get_endpoint_channel_name)
  - audio_status reports the device latencies
  - count_buffer_sizes takes the devices, like count_sample_rates, and lists only what
    they accept where the platform can tell
  - ids are opaque (above)
  new functions are at the end of the struct. draft/3 is no longer answered.
*/

#ifdef __cplusplus
extern "C"
{
#endif

  static const CLAP_CONSTEXPR char CLAP_WRAPPER_STANDALONE_FEATURES[] =
      "clap-wrapper.standalone-features.draft/5";

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

    // the device channel, 0-based, the plugin's first main input channel is fed from and
    // its first main output channel is sent to; the plugin's further channels take the
    // device channels after it. ignored unless supports_channel_selection
    uint32_t input_first_channel;
    uint32_t output_first_channel;
  } clap_wrapper_standalone_audio_config_t;

  typedef struct clap_wrapper_standalone_audio_status
  {
    // true from a request_audio_config until the standalone has applied it
    bool request_pending;

    // false if no stream is open, e.g. no output device attached or the open failed
    bool running;

    // see set_input_muted
    bool input_muted;

    // the api the stream is on, as in clap_wrapper_standalone_audio_api.id
    char api_id[CLAP_NAME_SIZE];

    // what the stream actually opened at, which may differ from what was requested.
    // 0 when not running
    uint32_t sample_rate;
    uint32_t buffer_size;
    uint32_t input_channels;
    uint32_t output_channels;

    // as in clap_wrapper_standalone_audio_config; 0 when not running
    uint32_t input_first_channel;
    uint32_t output_first_channel;

    // frames between the device's converters and the plugin, each way, as the device
    // reports them, buffer not included. 0 when not running or not known
    uint32_t input_latency;
    uint32_t output_latency;

    // the error from the most recent open, "" if it succeeded. survives until the
    // next open attempt, so a plugin polling after a request sees why it failed
    char last_error[CLAP_PATH_SIZE];
  } clap_wrapper_standalone_audio_status_t;

  enum clap_wrapper_standalone_appearance
  {
    // follow the system's light or dark setting, as it changes
    CLAP_WRAPPER_STANDALONE_APPEARANCE_AUTO = 0,
    CLAP_WRAPPER_STANDALONE_APPEARANCE_LIGHT = 1,
    CLAP_WRAPPER_STANDALONE_APPEARANCE_DARK = 2,
  };

  enum clap_wrapper_standalone_state_persistence
  {
    // the plugin's state is saved when the standalone quits and loaded back at the next
    // launch, over the state the plugin has after init(). the default
    CLAP_WRAPPER_STANDALONE_STATE_PERSIST_SESSION = 0,

    // every launch starts from the state the plugin has after init(), and nothing is
    // saved on quit. a session saved before is neither loaded nor deleted
    CLAP_WRAPPER_STANDALONE_STATE_PERSIST_NONE = 1,
  };

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
    // non-modal: returns once the window is up, not when the user closes it. the
    // window has a midi input section only for a plugin with a note input port, and
    // a midi output section only for one with a note output port.
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

    // snapshots the buffer sizes, in frames, a stream on these devices could ask for, ids
    // as in count_sample_rates: the standalone's list, narrowed to the sizes both devices
    // accept where the platform can tell. a device may still grant another size; the
    // standalone asks for the size and get_audio_status reports what it got
    // [main-thread]
    uint32_t(CLAP_ABI *count_buffer_sizes)(const clap_host_t *host, const char *output_id,
                                           const char *input_id);

    // 0 if index is out of range of the last count_buffer_sizes snapshot
    // [main-thread]
    uint32_t(CLAP_ABI *get_buffer_size)(const clap_host_t *host, uint32_t index);

    /* -- status -- */

    // returns false if unsupported
    // [main-thread]
    bool(CLAP_ABI *get_audio_status)(const clap_host_t *host,
                                     clap_wrapper_standalone_audio_status_t *status);

    /* -- input mute -- */

    // [main-thread]
    bool(CLAP_ABI *supports_input_mute)(const clap_host_t *host);

    // feeds the plugin silence in place of the input device, from the next block. the
    // stream stays open and the plugin stays active, so this is cheap and click-free
    // to toggle. lets a plugin which sees a first launch, or a changed device, mute
    // before a laptop's built-in mic and speakers can feed back, show the settings
    // screen, and unmute once the user has chosen. not persisted: every launch starts
    // unmuted. called from init() it is in effect before the first block is processed.
    // returns false if unsupported
    // [main-thread]
    bool(CLAP_ABI *set_input_muted)(const clap_host_t *host, bool muted);

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

    /* -- channels (draft/4) -- */

    // whether request_audio_config honours *_first_channel
    // [main-thread]
    bool(CLAP_ABI *supports_channel_selection)(const clap_host_t *host);

    // the name of a channel of an audio endpoint in the last count_endpoints snapshot of
    // kind, channel 0-based: "Input 3" where the device names none. returns false if
    // either index is out of range, kind is not an audio kind, or it is unsupported
    // [main-thread]
    bool(CLAP_ABI *get_endpoint_channel_name)(const clap_host_t *host, uint32_t kind, uint32_t index,
                                              uint32_t channel, char name[CLAP_NAME_SIZE]);

    /* -- the configuration screen's appearance (draft/4) -- */

    // bit (1 << appearance) for each clap_wrapper_standalone_appearance the configuration
    // screen can take; 0 if it cannot be set
    // [main-thread]
    uint32_t(CLAP_ABI *supported_configuration_screen_appearances)(const clap_host_t *host);

    // takes effect at once, on an open screen too, and on the standalone's own alerts
    // where the system lets them be drawn so (macos; windows' message boxes cannot). a
    // plugin that sets the appearance owns it: the screen offers no choice of its own for
    // the rest of the session, and the value is not saved - set it again, say from
    // init(), on every launch. returns false if unsupported
    // [main-thread]
    bool(CLAP_ABI *set_configuration_screen_appearance)(const clap_host_t *host, uint32_t appearance);

    // the appearance in effect, whoever chose it
    // [main-thread]
    uint32_t(CLAP_ABI *get_configuration_screen_appearance)(const clap_host_t *host);

    /* -- the plugin's state across launches (draft/5) -- */

    // bit (1 << persistence) for each clap_wrapper_standalone_state_persistence this
    // standalone can follow; 0 where it has nowhere to keep a session
    // [main-thread]
    uint32_t(CLAP_ABI *supported_plugin_state_persistence)(const clap_host_t *host);

    // called from init(), it decides whether this launch loads the last session's state;
    // called later, only whether this session's is saved on quit. the value is not saved -
    // set it again, say from init(), on every launch. the launch state the standalone
    // keeps for resetting, and a state the user saves to or opens from a file, are not
    // affected. returns false if unsupported
    // [main-thread]
    bool(CLAP_ABI *set_plugin_state_persistence)(const clap_host_t *host, uint32_t persistence);

    // the clap_wrapper_standalone_state_persistence in effect
    // [main-thread]
    uint32_t(CLAP_ABI *get_plugin_state_persistence)(const clap_host_t *host);
  } clap_wrapper_standalone_features_t;

  // how the user left the audio/midi configuration screen
  enum clap_wrapper_standalone_configuration_result
  {
    // they closed it without applying anything: cancel, the window's close button, or
    // any other way out of a screen in which nothing was chosen
    CLAP_WRAPPER_STANDALONE_CONFIGURATION_CANCELLED = 0,

    // they applied a configuration: confirmed one with the screen's OK button.
    // get_audio_status says what the stream runs at. a configuration that does not
    // open is neither kept nor reported - the screen stays up for another choice - so
    // this also means it is running
    CLAP_WRAPPER_STANDALONE_CONFIGURATION_APPLIED = 1,
  };

  // declared, not included: a plugin's own source tree may well have a clap/plugin.h
  // ahead of the sdk's on its include path
  struct clap_plugin;

  typedef struct clap_wrapper_plugin_standalone_features
  {
    // the user closed the standalone's audio/midi configuration screen, however it
    // was opened: by show_audio_midi_configuration_screen or from the standalone's
    // own menu. result is a clap_wrapper_standalone_configuration_result. called once
    // per close, after the screen is gone, and never from inside a call the plugin
    // is making
    // [main-thread]
    void(CLAP_ABI *audio_midi_configuration_screen_closed)(const struct clap_plugin *plugin,
                                                           uint32_t result);
  } clap_wrapper_plugin_standalone_features_t;

  /*
    clap_wrapper_plugin_factory_standalone_settings  (DRAFT)

    a factory a plugin's entry returns from get_factory under this id, to name the base
    folder the standalone keeps its settings under. the standalone asks for it once, after
    the entry's init() and before it creates the plugin, so the folder is the entry's to
    choose, not an instance's.

    under the base folder the standalone keeps one folder per plugin id, as it does under
    its own, and puts its files for that plugin there: the audio and midi setup, the state
    saved on quit, and the launch state it keeps for resetting. a plugin need not create
    anything; the standalone creates what is missing.
  */

  static const CLAP_CONSTEXPR char CLAP_WRAPPER_PLUGIN_FACTORY_STANDALONE_SETTINGS[] =
      "clap-wrapper.plugin-factory-standalone-settings.draft/1";

  typedef struct clap_wrapper_plugin_factory_standalone_settings
  {
    // writes the base folder, an absolute path in utf-8, to path, null-terminated within
    // path_capacity bytes. returns false, or leaves path empty, for the standalone's own
    // base folder. a folder which is not absolute or cannot be created is logged, and the
    // standalone's own is used
    // [main-thread]
    bool(CLAP_ABI *get_settings_base_folder)(
        const struct clap_wrapper_plugin_factory_standalone_settings *factory, char *path,
        uint32_t path_capacity);
  } clap_wrapper_plugin_factory_standalone_settings_t;

#ifdef __cplusplus
}
#endif

#endif
