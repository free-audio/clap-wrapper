#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wlanguage-extension-token"

#import "auv3_audiounit.h"
#include "auv3_parameters.h"
#include "process.h"

#include "clap_proxy.h"
#include "detail/clap/fsutil.h"
#include "detail/os/osutil.h"
#include "detail/shared/fixedqueue.h"
#include "detail/clap/automation.h"

#include <os/log.h>
#include <unistd.h>
#include <iostream>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <dlfcn.h>

static os_log_t _auv3Log()
{
  static os_log_t log = os_log_create("org.clap-wrapper.auv3", "wrapper");
  return log;
}
#define AUV3LOG(...) os_log(_auv3Log(), __VA_ARGS__)
#define AUV3ERR(...) os_log_error(_auv3Log(), __VA_ARGS__)

// drainParameterQueue and the bypass setter reflect already-delivered values
// into the AUParameter tree via setValue:originator:. The tree's
// implementorValueObserver fires synchronously on the calling thread for
// those sets too (originator tokens only suppress token-registered
// observers, never the implementor hooks) — without this guard every
// plugin-originated change would be echoed back into the plugin as a new
// host change. thread_local so a genuine host set on another thread is
// never suppressed.
static thread_local bool s_suppressParamObserverEcho = false;

// Forward-declare private methods used by C++ code before the @implementation
@interface ClapAUv3AudioUnit ()
- (void)_replaceParameterTree;
- (void)_notifyParameterValuesChanged;
- (void)_wireParameterObserver;
- (void)_applyGUISizeWidth:(uint32_t)width height:(uint32_t)height;
@end

// -----------------------------------------------------------------------
// C++ implementation detail bridging IHost, IAutomation, and IPlugObject
// -----------------------------------------------------------------------

namespace free_audio::auv3_wrapper
{

class queueEvent
{
 public:
  typedef enum class type
  {
    editstart,
    editvalue,
    editend,
  } type_t;
  type_t _type;
  union
  {
    clap_id _id;
    clap_event_param_value_t _value;
  } _data;
};

class AUv3ImplDetail : public Clap::IHost, public Clap::IAutomation, public os::IPlugObject
{
 public:
  AUv3ImplDetail() : _os_attached([this] { os::attach(this); }, [this] { os::detach(this); })
  {
  }

  ~AUv3ImplDetail() override
  {
    AUV3LOG("~AUv3ImplDetail: destructor entered (plugin=%{public}s)", _plugin ? "valid" : "null");
    if (_plugin)
    {
      auto mainGuard = _plugin->AlwaysMainThread();
      AUV3LOG("~AUv3ImplDetail: calling _plugin->terminate()");
      _plugin->terminate();
      AUV3LOG("~AUv3ImplDetail: calling _plugin.reset()");
      _plugin.reset();
      AUV3LOG("~AUv3ImplDetail: plugin teardown complete");
    }
  }

  // CLAP plugin state
  std::shared_ptr<Clap::Plugin> _plugin;
  std::unique_ptr<Clap::AUv3::ProcessAdapter> _processAdapter;
  const clap_plugin_descriptor_t *_desc = nullptr;

  // Render-block handshake. The render thread reaches the adapter ONLY
  // through _processAdapterLive; deallocateRenderResources unpublishes it
  // and then drains _renderInFlight before the adapter is freed, so a
  // render callback overlapping deallocation can never use-after-free.
  // seq_cst throughout — one RMW per buffer is not worth weaker ordering.
  std::atomic<Clap::AUv3::ProcessAdapter *> _processAdapterLive{nullptr};
  std::atomic<int> _renderInFlight{0};

  // Audio bus info
  struct BusInfo
  {
    uint32_t channelCount;
    std::string name;
  };
  std::vector<BusInfo> _inputBusInfos;
  std::vector<BusInfo> _outputBusInfos;

  // MIDI
  uint32_t _midi_preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
  uint32_t _midi_supported_dialects = 0;
  bool _midi_wants_midi_input = false;
  std::vector<NSString *> _midiOutputNames;

  // Parameters
  AUParameterTree *_parameterTree = nil;
  clap_id _bypassParamId = CLAP_INVALID_ID;  // set if CLAP plugin has a bypass parameter
  // Observer token used as 'originator' when pushing parameter changes to the host.
  // This prevents the host from echoing the change back to our implementorValueObserver.
  AUParameterObserverToken _parameterObserverToken = nullptr;

  // Hosting
  std::string _clapname;
  std::string _clapid;
  int _idx = 0;
  os::State _os_attached;
  std::string _hostname = "CLAP-as-AUv3";
  std::atomic<bool> _initialized{false};
  std::atomic_bool _requestUICallback{false};
  dispatch_source_t _idleTimer = nullptr;

  // Back-reference to the ObjC audio unit (weak to avoid retain cycle)
  __weak ClapAUv3AudioUnit *_audioUnit = nil;

  // The native view (NSView on macOS, UIView on iOS) that the CLAP GUI
  // is parented to. Set by createGUIInView:.
  __weak CLAPWRAP_ViewClass *_guiParentView = nil;

  // The view controller that owns the GUI — needed for gui_request_resize
  // to set preferredContentSize (the only legal AUv3 host communication path).
  __weak ClapAUv3ViewController *_viewController = nil;

  // Cached parameter values — avoids calling params->get_value() on every
  // provider callback (wrong thread, expensive via XPC). Updated on set/flush
  // and, via the audio→UI queue drain, when the plugin emits parameter output
  // events. These maps are only ever touched on the main and XPC threads —
  // never the render thread (it reads the ProcessAdapter's private cookie
  // snapshot instead) — and every access is serialized by _paramCacheMutex:
  // unordered_map mutation under concurrency is bucket corruption, not a
  // benign stale read. The mutex is never held across a plugin call.
  std::mutex _paramCacheMutex;
  std::unordered_map<clap_id, double> _paramValueCache;
  std::unordered_map<clap_id, void *> _paramCookieCache;

  // Cached latency in samples — queried on init and when the plugin calls
  // latency_changed(). The AUv3 host reads the latency property from any
  // thread, so we cache it to avoid calling into the plugin on the wrong thread.
  uint32_t _cachedLatencySamples = 0;

  // Queue for audio -> UI thread parameter notifications
  ClapWrapper::detail::shared::fixedqueue<queueEvent, 8192> _queueToUI;

  // CLAP timer extension support — mirrors VST3/AAX TimerObject pattern
  struct TimerObject
  {
    uint32_t period = 0;  // 0 = unused slot (available for reuse)
    uint64_t nexttick = 0;
    clap_id timer_id = 0;
  };
  std::vector<TimerObject> _timerObjects;

  // --- IHost ---
  void mark_dirty() override
  {
    AUV3LOG("IHost::mark_dirty() called");
  }
  void restartPlugin() override
  {
    AUV3LOG("IHost::restartPlugin() called");
  }

  void request_callback() override
  {
    // Just set the flag. The main-queue idle timer will service it between
    // render cycles. Never call on_main_thread() synchronously or from
    // the render thread — JUCE holds locks in on_main_thread() that
    // process() also needs, causing deadlock.
    _requestUICallback = true;
  }

  void startIdleTimer()
  {
    if (_idleTimer) return;
    _idleTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(_idleTimer, DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC, 1 * NSEC_PER_MSEC);

    auto plugin = _plugin;
    auto *flag = &_requestUICallback;
    auto *processing = &_initialized;  // true between start_processing/stop_processing
    auto *self = this;
    dispatch_source_set_event_handler(_idleTimer, ^{
      // Drain the parameter automation queue (Touch/Value/Release → host).
      // This is safe even while processing — it only touches AUParameter
      // objects on the main queue, no CLAP plugin calls.
      self->drainParameterQueue();

      // Fire CLAP timers — safe while processing since timer callbacks
      // run on the main thread, not the audio thread.
      self->fireTimers();

      // Do NOT call on_main_thread() while the plugin is processing.
      // JUCE's on_main_thread() acquires locks that process() also needs —
      // calling both concurrently (main thread vs render thread) deadlocks.
      if (processing->load()) return;

      // Service request_callback
      if (flag->exchange(false))
      {
        auto guard = plugin->AlwaysMainThread();
        plugin->_plugin->on_main_thread(plugin->_plugin);
      }
    });
    dispatch_resume(_idleTimer);
  }

  void stopIdleTimer()
  {
    if (_idleTimer)
    {
      dispatch_source_cancel(_idleTimer);
      _idleTimer = nullptr;
    }
    _timerObjects.clear();
  }

  void setupWrapperSpecifics(const clap_plugin_t *plugin) override
  {
    // AUv3-specific extensions could be queried here
  }

  void setupAudioBusses(const clap_plugin_t *plugin,
                        const clap_plugin_audio_ports_t *audioports) override
  {
    _inputBusInfos.clear();
    _outputBusInfos.clear();

    auto numIn = audioports->count(plugin, true);
    auto numOut = audioports->count(plugin, false);

    for (decltype(numIn) i = 0; i < numIn; ++i)
    {
      clap_audio_port_info_t info;
      if (audioports->get(plugin, i, true, &info))
      {
        _inputBusInfos.push_back({info.channel_count, info.name});
      }
    }

    for (decltype(numOut) i = 0; i < numOut; ++i)
    {
      clap_audio_port_info_t info;
      if (audioports->get(plugin, i, false, &info))
      {
        _outputBusInfos.push_back({info.channel_count, info.name});
      }
    }
  }

  void setupMIDIBusses(const clap_plugin_t *plugin, const clap_plugin_note_ports_t *noteports) override
  {
    if (!noteports) return;

    auto numMIDIIn = noteports->count(plugin, true);
    auto numMIDIOut = noteports->count(plugin, false);

    _midi_wants_midi_input = (numMIDIIn > 0);
    if (numMIDIIn > 0)
    {
      clap_note_port_info_t info;
      if (noteports->get(plugin, 0, true, &info))
      {
        _midi_preferred_dialect = info.preferred_dialect;
        _midi_supported_dialects = info.supported_dialects;
      }
    }

    _midiOutputNames.clear();
    for (decltype(numMIDIOut) i = 0; i < numMIDIOut; ++i)
    {
      clap_note_port_info_t info;
      if (noteports->get(plugin, i, false, &info))
      {
        _midiOutputNames.push_back([NSString stringWithUTF8String:info.name]);
      }
    }
  }

  void setupParameters(const clap_plugin_t *plugin, const clap_plugin_params_t *params) override
  {
    auto result = Clap::AUv3::createParameterTree(plugin, params);
    _parameterTree = result.tree;
    _bypassParamId = result.bypassParamId;

    // Populate the parameter value and cookie caches with initial values.
    // Build locally, then publish under the lock so concurrent readers
    // never see a half-built map.
    if (params)
    {
      std::unordered_map<clap_id, double> values;
      std::unordered_map<clap_id, void *> cookies;
      uint32_t numParams = params->count(plugin);
      for (uint32_t i = 0; i < numParams; ++i)
      {
        clap_param_info_t info;
        if (params->get_info(plugin, i, &info))
        {
          double value = 0;
          if (params->get_value(plugin, info.id, &value))
            values[info.id] = value;
          else
            values[info.id] = info.default_value;
          cookies[info.id] = info.cookie;
        }
      }
      std::lock_guard<std::mutex> lock(_paramCacheMutex);
      _paramValueCache = std::move(values);
      _paramCookieCache = std::move(cookies);
    }
  }

  void param_rescan(clap_param_rescan_flags flags) override
  {
    AUV3LOG("IHost::param_rescan(flags=0x%x) called", (unsigned)flags);
    if (!_plugin || !_plugin->_ext._params) return;

    auto mainGuard = _plugin->AlwaysMainThread();
    auto *params = _plugin->_ext._params;
    auto *plug = _plugin->_plugin;

    if (flags & (CLAP_PARAM_RESCAN_ALL | CLAP_PARAM_RESCAN_INFO))
    {
      // AUParameter properties (name, range, flags) are immutable — rebuild the entire tree.
      auto rescanResult = Clap::AUv3::createParameterTree(plug, params);
      _parameterTree = rescanResult.tree;
      _bypassParamId = rescanResult.bypassParamId;

      // Immediately replace the value provider with the cached version —
      // createParameterTree() wires a provider that calls get_value() directly,
      // which fails the thread check if called from the render thread.
      auto *cache = &_paramValueCache;
      auto *cacheMutex = &_paramCacheMutex;
      _parameterTree.implementorValueProvider = ^AUValue(AUParameter *param) {
        std::lock_guard<std::mutex> lock(*cacheMutex);
        auto it = cache->find((clap_id)param.address);
        if (it != cache->end()) return (AUValue)it->second;
        return (AUValue)0.0;
      };

      // Refresh value and cookie caches — build locally, publish under the lock
      std::unordered_map<clap_id, double> values;
      std::unordered_map<clap_id, void *> cookies;
      uint32_t n = params->count(plug);
      for (uint32_t i = 0; i < n; ++i)
      {
        clap_param_info_t info;
        if (params->get_info(plug, i, &info))
        {
          double value = 0;
          if (params->get_value(plug, info.id, &value))
            values[info.id] = value;
          else
            values[info.id] = info.default_value;
          cookies[info.id] = info.cookie;
        }
      }
      {
        std::lock_guard<std::mutex> lock(_paramCacheMutex);
        _paramValueCache = std::move(values);
        _paramCookieCache = std::move(cookies);
      }

      // Notify AUv3 host via KVO — must be on main thread
      __strong auto au = _audioUnit;
      if (au)
      {
        dispatch_async(dispatch_get_main_queue(), ^{
          [au _replaceParameterTree];
        });
      }
    }
    else if (flags & CLAP_PARAM_RESCAN_VALUES)
    {
      // Just refresh cached values — tree structure is unchanged. Collect
      // first so the lock is not held across get_value() calls.
      std::vector<std::pair<clap_id, double>> values;
      uint32_t n = params->count(plug);
      values.reserve(n);
      for (uint32_t i = 0; i < n; ++i)
      {
        clap_param_info_t info;
        if (params->get_info(plug, i, &info))
        {
          double value = 0;
          if (params->get_value(plug, info.id, &value)) values.emplace_back(info.id, value);
        }
      }
      {
        std::lock_guard<std::mutex> lock(_paramCacheMutex);
        for (auto &v : values) _paramValueCache[v.first] = v.second;
      }

      // Notify host that values changed
      __strong auto au = _audioUnit;
      if (au)
      {
        dispatch_async(dispatch_get_main_queue(), ^{
          [au _notifyParameterValuesChanged];
        });
      }
    }

    // CLAP_PARAM_RESCAN_TEXT needs no action — implementorStringFromValueCallback
    // already calls plugin->value_to_text() on each invocation.
  }

  void param_clear(clap_id param, clap_param_clear_flags flags) override
  {
    AUV3LOG("IHost::param_clear(param=%u, flags=0x%x) called", (unsigned)param, (unsigned)flags);
  }

  void param_request_flush() override
  {
    AUV3LOG("IHost::param_request_flush() called");
  }

  void latency_changed() override
  {
    if (_plugin && _plugin->_ext._latency)
    {
      auto mainGuard = _plugin->AlwaysMainThread();
      _cachedLatencySamples = _plugin->_ext._latency->get(_plugin->_plugin);
      AUV3LOG("IHost::latency_changed() -> %u samples", _cachedLatencySamples);

      // Notify the AUv3 host via KVO so it re-reads the latency property
      if (_audioUnit)
      {
        [_audioUnit willChangeValueForKey:@"latency"];
        [_audioUnit didChangeValueForKey:@"latency"];
      }
    }
  }

  void tail_changed() override
  {
    AUV3LOG("IHost::tail_changed() called");
  }

  bool gui_can_resize() override
  {
    if (_plugin && _plugin->_ext._gui)
    {
      auto mainGuard = _plugin->AlwaysMainThread();
      return _plugin->_ext._gui->can_resize(_plugin->_plugin);
    }
    return false;
  }

  bool gui_request_resize(uint32_t width, uint32_t height) override
  {
    // Communicate size changes through the AUv3 protocol: set preferredContentSize
    // on the view controller. The host decides the final size.
    //
    // Go through the ObjC audio unit, captured weakly: this C++ object is
    // owned by it, so a raw `this` in the deferred block would dangle if
    // the AU is deallocated before the main queue runs the block.
    __weak ClapAUv3AudioUnit *weakAU = _audioUnit;
    dispatch_async(dispatch_get_main_queue(), ^{
      __strong ClapAUv3AudioUnit *au = weakAU;
      if (au) [au _applyGUISizeWidth:width height:height];
    });
    return true;
  }

  bool gui_request_show() override
  {
    return false;
  }
  bool gui_request_hide() override
  {
    return false;
  }

  bool register_timer(uint32_t period_ms, clap_id *timer_id) override
  {
    if (period_ms < 30) period_ms = 30;

    // Reuse an unused slot
    for (size_t i = 0; i < _timerObjects.size(); ++i)
    {
      auto &to = _timerObjects[i];
      if (to.period == 0)
      {
        to.timer_id = static_cast<clap_id>(i + 1000);
        to.period = period_ms;
        to.nexttick = os::getTickInMS() + period_ms;
        *timer_id = to.timer_id;
        return true;
      }
    }

    // Create new slot
    auto newid = static_cast<clap_id>(_timerObjects.size() + 1000);
    _timerObjects.push_back({period_ms, os::getTickInMS() + period_ms, newid});
    *timer_id = newid;
    return true;
  }

  bool unregister_timer(clap_id timer_id) override
  {
    for (auto &to : _timerObjects)
    {
      if (to.timer_id == timer_id)
      {
        to.period = 0;
        to.nexttick = 0;
        return true;
      }
    }
    return false;
  }

  void fireTimers()
  {
    if (_timerObjects.empty() || !_plugin || !_plugin->_ext._timer) return;

    auto now = os::getTickInMS();
    for (auto &to : _timerObjects)
    {
      if (to.period > 0 && to.nexttick <= now)
      {
        to.nexttick = now + to.period;
        auto guard = _plugin->AlwaysMainThread();
        _plugin->_ext._timer->on_timer(_plugin->_plugin, to.timer_id);
      }
    }
  }

  const char *host_get_name() override
  {
    NSBundle *mainBundle = [NSBundle mainBundle];
    if (mainBundle)
    {
      NSString *name = [mainBundle objectForInfoDictionaryKey:@"CFBundleName"];
      NSString *version = [mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
      if (name)
      {
        _hostname = [name UTF8String];
        if (version)
        {
          _hostname += " ";
          _hostname += [version UTF8String];
        }
        _hostname += " (CLAP-as-AUv3)";
      }
    }
    return _hostname.c_str();
  }

  bool track_info_get(clap_track_info_t *info) override
  {
    return false;
  }

  bool supportsContextMenu() const override
  {
    return false;
  }
  bool context_menu_populate(const clap_context_menu_target_t *target,
                             const clap_context_menu_builder_t *builder) override
  {
    return false;
  }
  bool context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id) override
  {
    return false;
  }
  bool context_menu_can_popup() override
  {
    return false;
  }
  bool context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index, int32_t x,
                          int32_t y) override
  {
    return false;
  }

  // --- IAutomation ---
  // These run on the render thread (plugin output events during process()),
  // so they must stay realtime-safe: no logging, no locks, no map mutation.
  // They only push to the lock-free queue; drainParameterQueue applies the
  // value to _paramValueCache on the main queue.
  void onBeginEdit(clap_id id) override
  {
    queueEvent evt;
    evt._type = queueEvent::type::editstart;
    evt._data._id = id;
    _queueToUI.push(evt);
  }

  void onPerformEdit(const clap_event_param_value_t *value) override
  {
    queueEvent evt;
    evt._type = queueEvent::type::editvalue;
    evt._data._value = *value;
    _queueToUI.push(evt);
  }

  void onEndEdit(clap_id id) override
  {
    queueEvent evt;
    evt._type = queueEvent::type::editend;
    evt._data._id = id;
    _queueToUI.push(evt);
  }

  // Drain the audio→UI parameter queue and forward automation events to the host.
  // Safe to call while processing — only touches AUParameter objects, no CLAP calls.
  void drainParameterQueue()
  {
    queueEvent evt;
    while (_queueToUI.pop(evt))
    {
      if (!_parameterTree) continue;

      // The setValue calls below re-enter implementorValueObserver on this
      // thread — suppress the echo (see s_suppressParamObserverEcho).
      s_suppressParamObserverEcho = true;

      switch (evt._type)
      {
        case queueEvent::type::editstart:
        {
          AUParameter *param = [_parameterTree parameterWithAddress:(AUParameterAddress)evt._data._id];
          if (param)
          {
            [param setValue:param.value
                 originator:_parameterObserverToken
                 atHostTime:0
                  eventType:AUParameterAutomationEventTypeTouch];
          }
          break;
        }
        case queueEvent::type::editvalue:
        {
          // Apply the plugin-side change to the value cache here (main queue)
          // — the render thread must not touch the map itself.
          {
            std::lock_guard<std::mutex> lock(_paramCacheMutex);
            _paramValueCache[evt._data._value.param_id] = evt._data._value.value;
          }
          AUParameter *param =
              [_parameterTree parameterWithAddress:(AUParameterAddress)evt._data._value.param_id];
          if (param)
          {
            [param setValue:(AUValue)evt._data._value.value
                 originator:_parameterObserverToken
                 atHostTime:0
                  eventType:AUParameterAutomationEventTypeValue];
          }
          // If this was the bypass parameter, notify KVO observers of shouldBypassEffect
          if (evt._data._value.param_id == _bypassParamId && _audioUnit)
          {
            [_audioUnit willChangeValueForKey:@"shouldBypassEffect"];
            [_audioUnit didChangeValueForKey:@"shouldBypassEffect"];
          }
          break;
        }
        case queueEvent::type::editend:
        {
          AUParameter *param = [_parameterTree parameterWithAddress:(AUParameterAddress)evt._data._id];
          if (param)
          {
            [param setValue:param.value
                 originator:_parameterObserverToken
                 atHostTime:0
                  eventType:AUParameterAutomationEventTypeRelease];
          }
          break;
        }
      }

      s_suppressParamObserverEcho = false;
    }
  }

  // Deliver a single parameter value to the plugin via params->flush().
  // Only legal while the plugin is NOT processing. The single construction
  // point for the one-shot event list used by the observer, the bypass
  // setter, and the post-deallocate queue drain.
  void flushParamValueWithCookie(clap_id id, double value, void *cookie)
  {
    if (!_plugin || !_plugin->_ext._params) return;

    clap_event_param_value_t ev = {};
    ev.header.size = sizeof(ev);
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.time = 0;
    ev.header.flags = 0;
    ev.param_id = id;
    ev.value = value;
    ev.port_index = -1;
    ev.key = -1;
    ev.channel = -1;
    ev.note_id = -1;
    ev.cookie = cookie;

    const clap_event_header_t *evPtr = &ev.header;
    clap_input_events_t in_events = {};
    in_events.ctx = &evPtr;
    in_events.size = [](const clap_input_events_t *) -> uint32_t { return 1; };
    in_events.get = [](const clap_input_events_t *list, uint32_t) -> const clap_event_header_t *
    { return *static_cast<const clap_event_header_t *const *>(list->ctx); };

    clap_output_events_t out_events = {};
    out_events.ctx = nullptr;
    out_events.try_push = [](const clap_output_events_t *, const clap_event_header_t *) -> bool
    { return true; };

    auto mainGuard = _plugin->AlwaysMainThread();
    _plugin->_ext._params->flush(_plugin->_plugin, &in_events, &out_events);
  }

  void flushParamValue(clap_id id, double value)
  {
    void *cookie = nullptr;
    {
      std::lock_guard<std::mutex> lock(_paramCacheMutex);
      auto it = _paramCookieCache.find(id);
      if (it != _paramCookieCache.end()) cookie = it->second;
    }
    flushParamValueWithCookie(id, value, cookie);
  }

  // --- IPlugObject ---
  void onIdle() override
  {
    if (!_plugin) return;

    if (_requestUICallback.exchange(false))
    {
      auto guard = _plugin->AlwaysMainThread();
      _plugin->_plugin->on_main_thread(_plugin->_plugin);
    }

    drainParameterQueue();
  }
};

}  // namespace free_audio::auv3_wrapper

// -----------------------------------------------------------------------
// Static CLAP library holder
// -----------------------------------------------------------------------

static Clap::Library _library;

// -----------------------------------------------------------------------
// ClapAUv3AudioUnit implementation
// -----------------------------------------------------------------------

@implementation ClapAUv3AudioUnit
{
  std::unique_ptr<free_audio::auv3_wrapper::AUv3ImplDetail> _impl;
  AUAudioUnitBusArray *_inputBusArray;
  AUAudioUnitBusArray *_outputBusArray;
  BOOL _renderResourcesAllocated;
}

- (instancetype)initWithComponentDescription:(AudioComponentDescription)componentDescription
                                     options:(AudioComponentInstantiationOptions)options
                                       error:(NSError **)outError
                                    clapName:(NSString *)clapName
                                      clapId:(NSString *)clapId
                                   clapIndex:(int)clapIndex
{
  AUV3LOG("initWithComponentDescription: entered (name=%{public}s id=%{public}s idx=%d)",
          [clapName UTF8String], clapId ? [clapId UTF8String] : "(nil)", clapIndex);
  AUV3LOG("initWithComponentDescription: thread=%{public}s",
          [NSThread.currentThread.name UTF8String] ?: "unnamed");

  self = [super initWithComponentDescription:componentDescription options:options error:outError];
  if (!self)
  {
    AUV3ERR("initWithComponentDescription: [super init] returned nil");
    return nil;
  }
  AUV3LOG("initWithComponentDescription: super init succeeded, self=%p", self);

  try
  {
    _impl = std::make_unique<free_audio::auv3_wrapper::AUv3ImplDetail>();
    _impl->_audioUnit = self;
    _impl->_clapname = [clapName UTF8String];
    _impl->_clapid = clapId ? [clapId UTF8String] : "";
    _impl->_idx = clapIndex;

    AUV3LOG("init: name='%{public}s' id='%{public}s' idx=%d", _impl->_clapname.c_str(),
            _impl->_clapid.c_str(), _impl->_idx);

    // When the wrapper is built with STATICALLY_LINKED_CLAP_ENTRY (iOS), the
    // CLAP's clap_entry global is in our own binary. Wire _library to it
    // directly so the filesystem / dlopen search below is skipped. On iOS
    // that search would fail anyway — there is no writable CLAP path and
    // app extensions can't dlopen arbitrary bundles.
#if STATICALLY_LINKED_CLAP_ENTRY
    {
      extern const clap_plugin_entry clap_entry;
      Dl_info dlinfo{};
      const char *pathHint = "";
      if (dladdr(reinterpret_cast<const void *>(&clap_entry), &dlinfo) && dlinfo.dli_fname)
      {
        pathHint = dlinfo.dli_fname;
      }
      _library.useStaticEntry(&clap_entry, pathHint);
      AUV3LOG("init: statically-linked clap_entry path='%{public}s' plugins=%zu", pathHint,
              _library.plugins.size());
    }
#endif

    // Load CLAP library
    if (!_library.hasEntryPoint())
    {
      AUV3LOG("init: library has no entry point, searching for CLAP");
      if (_impl->_clapname.empty())
      {
        AUV3ERR("init: _clapname empty and no internal entry point");
        if (outError)
          *outError = [NSError errorWithDomain:@"ClapAUv3"
                                          code:-1
                                      userInfo:@{NSLocalizedDescriptionKey : @"CLAP name is empty"}];
        return nil;
      }

      auto csp = Clap::getValidCLAPSearchPaths();
      for (const auto &p : csp)
      {
        AUV3LOG("init: search path: %{public}s", p.u8string().c_str());
      }

      auto it = std::find_if(csp.begin(), csp.end(),
                             [&](const auto &cs)
                             {
                               auto fp = cs / (_impl->_clapname + ".clap");
                               AUV3LOG("init: trying %{public}s", fp.u8string().c_str());
                               return fs::is_directory(fp) && _library.load(fp);
                             });

      if (it != csp.end())
      {
        AUV3LOG("init: loaded CLAP from %{public}s", it->u8string().c_str());
      }
      else
      {
        AUV3ERR("init: cannot load CLAP '%{public}s'", _impl->_clapname.c_str());
        if (outError)
          *outError =
              [NSError errorWithDomain:@"ClapAUv3"
                                  code:-2
                              userInfo:@{NSLocalizedDescriptionKey : @"Cannot load CLAP plugin"}];
        return nil;
      }
    }
    else
    {
      AUV3LOG("init: library already has entry point, skipping search");
    }

    // Find the plugin descriptor
    AUV3LOG("init: finding plugin descriptor (clapid='%{public}s' idx=%d, library has %zu plugins)",
            _impl->_clapid.c_str(), _impl->_idx, _library.plugins.size());
    if (!_impl->_clapid.empty())
    {
      for (auto *d : _library.plugins)
      {
        if (strcmp(d->id, _impl->_clapid.c_str()) == 0)
        {
          _impl->_desc = d;
        }
      }
    }
    else if (_impl->_idx >= 0 && _impl->_idx < (int)_library.plugins.size())
    {
      _impl->_desc = _library.plugins[_impl->_idx];
    }

    if (!_impl->_desc)
    {
      AUV3ERR("init: cannot determine plugin description");
      if (outError)
        *outError = [NSError
            errorWithDomain:@"ClapAUv3"
                       code:-3
                   userInfo:@{NSLocalizedDescriptionKey : @"Cannot find CLAP plugin descriptor"}];
      return nil;
    }

    AUV3LOG("init: found descriptor id='%{public}s' name='%{public}s' version='%{public}s'",
            _impl->_desc->id, _impl->_desc->name, _impl->_desc->version);

    // Create the plugin instance
    AUV3LOG("init: creating plugin instance via factory");
    _impl->_plugin =
        Clap::Plugin::createInstance(_library._pluginFactory, _impl->_desc->id, _impl.get());
    if (!_impl->_plugin)
    {
      AUV3ERR("init: factory returned null plugin instance");
      if (outError)
        *outError = [NSError
            errorWithDomain:@"ClapAUv3"
                       code:-4
                   userInfo:@{NSLocalizedDescriptionKey : @"CLAP plugin instance creation failed"}];
      return nil;
    }
    AUV3LOG("init: plugin instance created successfully");

    AUV3LOG("init: calling plugin->initialize()");
    _impl->_plugin->initialize();

    // Cache the initial latency so the AUv3 host can read it from any thread.
    if (_impl->_plugin->_ext._latency)
    {
      _impl->_cachedLatencySamples = _impl->_plugin->_ext._latency->get(_impl->_plugin->_plugin);
      AUV3LOG("init: initial latency = %u samples", _impl->_cachedLatencySamples);
    }

    // Start the idle timer on the main queue. This services request_callback()
    // (on_main_thread) between render cycles. We don't use the global os::attach
    // mechanism — its CFRunLoopTimer is unreliable in out-of-process AUv3.
    AUV3LOG("init: starting idle timer on main queue");
    _impl->startIdleTimer();

    // Build audio bus arrays from the CLAP audio port info
    AUV3LOG("init: building bus arrays (inputs=%zu outputs=%zu)", _impl->_inputBusInfos.size(),
            _impl->_outputBusInfos.size());
    [self _buildBusArrays];

    _renderResourcesAllocated = NO;

    // Wire up parameter observer so parameter changes reach the CLAP plugin
    // both during rendering (via process adapter) and outside rendering (via flush).
    if (_impl->_parameterTree)
    {
      AUV3LOG("init: wiring parameter observer");
      [self _wireParameterObserver];
    }

    AUV3LOG("init: completed successfully");
  }
  catch (int e)
  {
    AUV3ERR("init: caught exception of type int: %d", e);
    if (outError)
      *outError =
          [NSError errorWithDomain:@"ClapAUv3"
                              code:e
                          userInfo:@{NSLocalizedDescriptionKey : @"C++ int exception during init"}];
    return nil;
  }
  catch (const std::exception &e)
  {
    AUV3ERR("init: caught std::exception: %{public}s", e.what());
    if (outError)
      *outError = [NSError
          errorWithDomain:@"ClapAUv3"
                     code:-99
                 userInfo:@{NSLocalizedDescriptionKey : [NSString stringWithUTF8String:e.what()]}];
    return nil;
  }
  catch (...)
  {
    AUV3ERR("init: caught unknown C++ exception");
    if (outError)
      *outError =
          [NSError errorWithDomain:@"ClapAUv3"
                              code:-98
                          userInfo:@{NSLocalizedDescriptionKey : @"Unknown C++ exception during init"}];
    return nil;
  }

  return self;
}

- (void)dealloc
{
  AUV3LOG("dealloc: entered (self=%p, thread=%{public}s)", self,
          [NSThread.currentThread.name UTF8String] ?: "unnamed");
  AUV3LOG("dealloc: _impl=%{public}s, _plugin=%{public}s", _impl ? "valid" : "null",
          (_impl && _impl->_plugin) ? "valid" : "null");

  if (_impl)
  {
    AUV3LOG("dealloc: stopping idle timer");
    _impl->stopIdleTimer();

    if (_impl->_parameterObserverToken && _impl->_parameterTree)
    {
      [_impl->_parameterTree removeParameterObserver:_impl->_parameterObserverToken];
      _impl->_parameterObserverToken = nullptr;
    }

    if (_impl->_plugin)
    {
      auto mainGuard = _impl->_plugin->AlwaysMainThread();
      AUV3LOG("dealloc: calling _plugin->terminate()");
      _impl->_plugin->terminate();
      AUV3LOG("dealloc: calling _plugin.reset()");
      _impl->_plugin.reset();
      AUV3LOG("dealloc: plugin teardown complete");
    }
  }
  AUV3LOG("dealloc: calling _impl.reset()");
  _impl.reset();
  AUV3LOG("dealloc: finished");
}

- (void)_buildBusArrays
{
  // Build input bus array
  NSMutableArray<AUAudioUnitBus *> *inputs = [NSMutableArray new];
  for (auto &busInfo : _impl->_inputBusInfos)
  {
    AVAudioFormat *format = [[AVAudioFormat alloc]
        initStandardFormatWithSampleRate:self.outputBusses.count > 0 ? 44100.0 : 44100.0
                                channels:busInfo.channelCount];
    if (format)
    {
      NSError *error = nil;
      AUAudioUnitBus *bus = [[AUAudioUnitBus alloc] initWithFormat:format error:&error];
      if (bus)
      {
        bus.name = [NSString stringWithUTF8String:busInfo.name.c_str()];
        [inputs addObject:bus];
      }
    }
  }
  _inputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                          busType:AUAudioUnitBusTypeInput
                                                           busses:inputs];

  // Build output bus array
  NSMutableArray<AUAudioUnitBus *> *outputs = [NSMutableArray new];
  for (auto &busInfo : _impl->_outputBusInfos)
  {
    AVAudioFormat *format =
        [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0 channels:busInfo.channelCount];
    if (format)
    {
      NSError *error = nil;
      AUAudioUnitBus *bus = [[AUAudioUnitBus alloc] initWithFormat:format error:&error];
      if (bus)
      {
        bus.name = [NSString stringWithUTF8String:busInfo.name.c_str()];
        [outputs addObject:bus];
      }
    }
  }
  _outputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self
                                                           busType:AUAudioUnitBusTypeOutput
                                                            busses:outputs];
}

- (void)_wireParameterObserver
{
  __weak __typeof(self) weakSelf = self;

  // Register a parameter observer to obtain a token. The token is used as
  // 'originator' in setValue:originator:atHostTime:eventType: so that
  // changes pushed from the CLAP plugin don't echo back through
  // implementorValueObserver (which would re-flush them to the plugin).
  // The observer block itself is intentionally empty — all host→plugin
  // value changes arrive via implementorValueObserver below.
  _impl->_parameterObserverToken =
      [_impl->_parameterTree tokenByAddingParameterObserver:^(AUParameterAddress address, AUValue value){
          // Intentionally empty — see comment above.
      }];

  _impl->_parameterTree.implementorValueObserver = ^(AUParameter *param, AUValue value) {
    __strong __typeof(weakSelf) strongSelf = weakSelf;
    if (!strongSelf || !strongSelf->_impl) return;
    if (!strongSelf->_impl->_plugin || !strongSelf->_impl->_plugin->_ext._params) return;

    // Echo of a value we ourselves just applied to the tree (queue drain,
    // bypass setter) — already delivered to the plugin; do not feed it back.
    if (s_suppressParamObserverEcho) return;

    // Always update the cache (and fetch the cookie in the same lock scope).
    // While rendering, also queue the change for the render thread:
    // flush() is forbidden while the plugin is processing and the adapter's
    // event vectors are render-thread-owned, so the SPSC queue (producers
    // serialized by this mutex) is the only legal delivery path — it is
    // drained as input events at the top of the next render cycle. The
    // rendering decision is made INSIDE the lock: allocate/deallocate flip
    // _renderResourcesAllocated under the same mutex, so we can never
    // flush while processing or queue on a freed adapter.
    clap_id pid = (clap_id)param.address;
    void *cookie = nullptr;
    bool queuedForRender = false;
    {
      std::lock_guard<std::mutex> lock(strongSelf->_impl->_paramCacheMutex);
      strongSelf->_impl->_paramValueCache[pid] = (double)value;
      auto cookieIt = strongSelf->_impl->_paramCookieCache.find(pid);
      if (cookieIt != strongSelf->_impl->_paramCookieCache.end()) cookie = cookieIt->second;

      if (strongSelf->_renderResourcesAllocated)
      {
        if (strongSelf->_impl->_processAdapter)
        {
          strongSelf->_impl->_processAdapter->queueParameterChange(pid, (double)value);
        }
        queuedForRender = true;
      }
    }
    if (queuedForRender) return;

    // Non-realtime path: push directly to the CLAP plugin via flush.
    strongSelf->_impl->flushParamValueWithCookie(pid, (double)value, cookie);
  };

  // Rewire the parameter tree callbacks. The provider uses the local cache
  // instead of calling params->get_value() (which requires main thread and is
  // expensive over XPC). String conversion still calls into the plugin with guards.
  auto plugin = _impl->_plugin;  // shared_ptr keeps it alive in the blocks
  auto *cache = &_impl->_paramValueCache;
  auto *cacheMutex = &_impl->_paramCacheMutex;

  _impl->_parameterTree.implementorValueProvider = ^AUValue(AUParameter *param) {
    std::lock_guard<std::mutex> lock(*cacheMutex);
    auto it = cache->find((clap_id)param.address);
    if (it != cache->end()) return (AUValue)it->second;
    return (AUValue)0.0;
  };

  _impl->_parameterTree.implementorStringFromValueCallback =
      ^NSString *(AUParameter *param, const AUValue *value) {
        auto guard = plugin->AlwaysMainThread();
        char buf[256];
        AUValue v = value ? *value : param.value;
        if (plugin->_ext._params->value_to_text(plugin->_plugin, (clap_id)param.address, (double)v, buf,
                                                sizeof(buf)))
        {
          return [NSString stringWithUTF8String:buf];
        }
        return [NSString stringWithFormat:@"%.3f", v];
      };

  _impl->_parameterTree.implementorValueFromStringCallback =
      ^AUValue(AUParameter *param, NSString *string) {
        auto guard = plugin->AlwaysMainThread();
        double value = 0;
        if (plugin->_ext._params->text_to_value(plugin->_plugin, (clap_id)param.address,
                                                [string UTF8String], &value))
        {
          return (AUValue)value;
        }
        return (AUValue)[string doubleValue];
      };
}

- (void)_replaceParameterTree
{
  AUV3LOG("_replaceParameterTree: firing KVO and re-wiring callbacks");

  // Remove the old observer token before the tree is replaced
  if (_impl->_parameterObserverToken && _impl->_parameterTree)
  {
    [_impl->_parameterTree removeParameterObserver:_impl->_parameterObserverToken];
    _impl->_parameterObserverToken = nullptr;
  }

  // Fire KVO so the host picks up the new tree
  [self willChangeValueForKey:@"parameterTree"];
  [self didChangeValueForKey:@"parameterTree"];

  // Re-wire the provider, observer, and string conversion callbacks
  [self _wireParameterObserver];
}

- (void)_notifyParameterValuesChanged
{
  AUV3LOG("_notifyParameterValuesChanged: firing KVO");
  // Pseudo-property documented in AUAudioUnit.h — hosts observe this
  // to know when all parameter values have been invalidated
  [self willChangeValueForKey:@"allParameterValues"];
  [self didChangeValueForKey:@"allParameterValues"];
}

// --- AUAudioUnit property overrides ---

- (AUAudioUnitBusArray *)inputBusses
{
  return _inputBusArray;
}

- (AUAudioUnitBusArray *)outputBusses
{
  return _outputBusArray;
}

- (AUParameterTree *)parameterTree
{
  if (_impl && _impl->_parameterTree)
  {
    return _impl->_parameterTree;
  }
  return [AUParameterTree createTreeWithChildren:@[]];
}

- (NSArray<NSString *> *)MIDIOutputNames
{
  if (_impl && !_impl->_midiOutputNames.empty())
  {
    NSMutableArray *names = [NSMutableArray new];
    for (auto &name : _impl->_midiOutputNames)
    {
      [names addObject:name];
    }
    return names;
  }
  return @[];
}

// Advertise the MIDI protocol we want our input delivered in. Returning
// kMIDIProtocol_2_0 tells the host to deliver MIDI 2.0 UMP (via
// AURenderEventMIDIEventList); we request it only when the hosted CLAP prefers
// the MIDI2 note dialect. Otherwise MIDI 1.0 flows through the richer byte path.
- (MIDIProtocolID)audioUnitMIDIProtocol
{
  if (_impl && _impl->_midi_preferred_dialect == CLAP_NOTE_DIALECT_MIDI2) return kMIDIProtocol_2_0;
  return kMIDIProtocol_1_0;
}

// Sample rate for time-based properties. A CLAP with no audio output ports
// (note effect → aumi) has an empty output bus array, and indexing it raises
// NSRangeException — fall back to the input side, then to 0 ("unknown").
- (double)_busSampleRate
{
  if (self.outputBusses.count > 0) return self.outputBusses[0].format.sampleRate;
  if (self.inputBusses.count > 0) return self.inputBusses[0].format.sampleRate;
  return 0;
}

- (NSTimeInterval)latency
{
  // Return the cached latency — queried on init and updated when the plugin
  // calls latency_changed(). Avoids calling into the plugin on the wrong thread.
  if (_impl && _impl->_cachedLatencySamples > 0)
  {
    double sr = [self _busSampleRate];
    if (sr > 0) return (double)_impl->_cachedLatencySamples / sr;
  }
  return 0;
}

- (NSTimeInterval)tailTime
{
  if (_impl && _impl->_plugin && _impl->_plugin->_ext._tail)
  {
    uint32_t samples = _impl->_plugin->_ext._tail->get(_impl->_plugin->_plugin);
    if (samples == UINT32_MAX) return INFINITY;
    double sr = [self _busSampleRate];
    if (sr > 0) return (double)samples / sr;
  }
  return 0;
}

- (NSArray<NSNumber *> *)channelCapabilities
{
  if (!_impl) return nil;

  // Build the channel capability pairs from CLAP audio port info, matching AUv2's
  // SupportedNumChannels() approach. Each pair is [inChannels, outChannels].
  // If there are no input ports, report 0 for input (generator/instrument).
  NSMutableArray<NSNumber *> *caps = [NSMutableArray new];

  std::vector<int> inCounts, outCounts;
  for (auto &bus : _impl->_inputBusInfos) inCounts.push_back((int)bus.channelCount);
  for (auto &bus : _impl->_outputBusInfos) outCounts.push_back((int)bus.channelCount);

  if (inCounts.empty()) inCounts.push_back(0);
  if (outCounts.empty()) outCounts.push_back(0);

  for (int ic : inCounts)
  {
    for (int oc : outCounts)
    {
      [caps addObject:@(ic)];
      [caps addObject:@(oc)];
    }
  }

  return caps;
}

- (BOOL)shouldChangeToFormat:(AVAudioFormat *)format forBus:(AUAudioUnitBus *)bus
{
  if (!_impl) return NO;

  uint32_t requestedChannels = format.channelCount;

  // Check input busses
  for (NSUInteger i = 0; i < self.inputBusses.count; ++i)
  {
    if (self.inputBusses[i] == bus)
    {
      if (i < _impl->_inputBusInfos.size())
      {
        BOOL ok = (requestedChannels == _impl->_inputBusInfos[i].channelCount);
        AUV3LOG("shouldChangeToFormat: input bus %lu requested %u ch, supported %u -> %{public}s",
                (unsigned long)i, requestedChannels, _impl->_inputBusInfos[i].channelCount,
                ok ? "YES" : "NO");
        return ok;
      }
      AUV3LOG("shouldChangeToFormat: input bus %lu out of range", (unsigned long)i);
      return NO;
    }
  }

  // Check output busses
  for (NSUInteger i = 0; i < self.outputBusses.count; ++i)
  {
    if (self.outputBusses[i] == bus)
    {
      if (i < _impl->_outputBusInfos.size())
      {
        BOOL ok = (requestedChannels == _impl->_outputBusInfos[i].channelCount);
        AUV3LOG("shouldChangeToFormat: output bus %lu requested %u ch, supported %u -> %{public}s",
                (unsigned long)i, requestedChannels, _impl->_outputBusInfos[i].channelCount,
                ok ? "YES" : "NO");
        return ok;
      }
      AUV3LOG("shouldChangeToFormat: output bus %lu out of range", (unsigned long)i);
      return NO;
    }
  }

  AUV3LOG("shouldChangeToFormat: bus not found, rejecting");
  return NO;
}

// --- State save/restore ---

- (NSDictionary<NSString *, id> *)fullState
{
  AUV3LOG("fullState (save): entered");
  NSMutableDictionary *state = [[super fullState] mutableCopy];
  if (!state) state = [NSMutableDictionary new];

  if (_impl && _impl->_plugin && _impl->_plugin->_ext._state)
  {
    Clap::StateMemento chunk;
    auto mainGuard = _impl->_plugin->AlwaysMainThread();
    if (_impl->_plugin->_ext._state->save(_impl->_plugin->_plugin, chunk))
    {
      NSData *clapState = [NSData dataWithBytes:chunk.data() length:chunk.size()];
      state[@"clapState"] = clapState;
      AUV3LOG("fullState (save): saved %zu bytes of CLAP state", (size_t)[clapState length]);
    }
    else
    {
      AUV3LOG("fullState (save): CLAP state save returned false");
    }
  }

  return state;
}

- (void)setFullState:(NSDictionary<NSString *, id> *)fullState
{
  AUV3LOG("setFullState (restore): entered");
  [super setFullState:fullState];

  if (_impl && _impl->_plugin && _impl->_plugin->_ext._state)
  {
    NSData *clapState = fullState[@"clapState"];
    if (clapState)
    {
      AUV3LOG("setFullState (restore): loading %zu bytes of CLAP state", (size_t)[clapState length]);
      Clap::StateMemento chunk;
      chunk.setData((const uint8_t *)[clapState bytes], [clapState length]);
      auto mainGuard = _impl->_plugin->AlwaysMainThread();
      _impl->_plugin->_ext._state->load(_impl->_plugin->_plugin, chunk);

      // Refresh the parameter cache after state restore — all values may have
      // changed. Collect first so the lock is not held across get_value() calls.
      if (_impl->_plugin->_ext._params)
      {
        auto *params = _impl->_plugin->_ext._params;
        auto *plug = _impl->_plugin->_plugin;
        std::vector<std::pair<clap_id, double>> values;
        uint32_t numParams = params->count(plug);
        values.reserve(numParams);
        for (uint32_t i = 0; i < numParams; ++i)
        {
          clap_param_info_t info;
          if (params->get_info(plug, i, &info))
          {
            double value = 0;
            if (params->get_value(plug, info.id, &value)) values.emplace_back(info.id, value);
          }
        }
        {
          std::lock_guard<std::mutex> lock(_impl->_paramCacheMutex);
          for (auto &v : values) _impl->_paramValueCache[v.first] = v.second;
        }
      }
      AUV3LOG("setFullState (restore): completed");
    }
    else
    {
      AUV3LOG("setFullState (restore): no clapState key in dictionary");
    }
  }
}

// --- Render resources ---

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError
{
  AUV3LOG("allocateRenderResources: entered (thread=%{public}s)",
          [NSThread.currentThread.name UTF8String] ?: "unnamed");

  if (![super allocateRenderResourcesAndReturnError:outError])
  {
    AUV3ERR("allocateRenderResources: [super] failed");
    return NO;
  }

  if (!_impl || !_impl->_plugin)
  {
    AUV3ERR("allocateRenderResources: plugin not initialized (_impl=%{public}s)",
            _impl ? "valid" : "null");
    if (outError)
      *outError = [NSError errorWithDomain:@"ClapAUv3"
                                      code:-10
                                  userInfo:@{NSLocalizedDescriptionKey : @"Plugin not initialized"}];
    return NO;
  }

  // Get sample rate from output bus format
  double sampleRate = 44100.0;
  if (self.outputBusses.count > 0)
  {
    sampleRate = self.outputBusses[0].format.sampleRate;
  }
  else if (self.inputBusses.count > 0)
  {
    sampleRate = self.inputBusses[0].format.sampleRate;
  }
  AUV3LOG("allocateRenderResources: sampleRate=%.0f maxFrames=%u", sampleRate,
          (unsigned)self.maximumFramesToRender);

  auto guarantee_mainthread = _impl->_plugin->AlwaysMainThread();

  AUV3LOG("allocateRenderResources: setting sample rate and block sizes");
  _impl->_plugin->setSampleRate(sampleRate);
  _impl->_plugin->setBlockSizes(1, self.maximumFramesToRender);

  // Collect channel counts
  std::vector<uint32_t> inputChs, outputChs;
  for (NSUInteger i = 0; i < self.inputBusses.count; ++i)
  {
    inputChs.push_back((uint32_t)self.inputBusses[i].format.channelCount);
  }
  for (NSUInteger i = 0; i < self.outputBusses.count; ++i)
  {
    outputChs.push_back((uint32_t)self.outputBusses[i].format.channelCount);
  }
  AUV3LOG("allocateRenderResources: input busses=%zu output busses=%zu", inputChs.size(),
          outputChs.size());

  // Create and set up the process adapter
  AUV3LOG("allocateRenderResources: creating process adapter");
  _impl->_processAdapter = std::make_unique<Clap::AUv3::ProcessAdapter>();
  _impl->_processAdapter->setupProcessing(
      (uint32_t)inputChs.size(), inputChs.empty() ? nullptr : inputChs.data(),
      (uint32_t)outputChs.size(), outputChs.empty() ? nullptr : outputChs.data(),
      _impl->_plugin->_plugin, _impl->_plugin->_ext._params, _impl.get(), self.maximumFramesToRender,
      _impl->_midi_preferred_dialect, _impl->_midi_supported_dialects);

  // Set transport state and musical context blocks
  _impl->_processAdapter->setTransportStateBlock(self.transportStateBlock);
  _impl->_processAdapter->setMusicalContextBlock(self.musicalContextBlock);

  // Snapshot the cookie cache for the render thread. The adapter owns a
  // private copy, so main-thread cache rebuilds can never race the render
  // path. Cookies only change on CLAP_PARAM_RESCAN_ALL/INFO, which requires
  // a deactivate/reactivate cycle — and that recreates this adapter anyway.
  {
    std::lock_guard<std::mutex> lock(_impl->_paramCacheMutex);
    _impl->_processAdapter->_cookieCache = _impl->_paramCookieCache;
  }

  // Set MIDI output block (legacy 3-byte path)
  _impl->_processAdapter->midiOutputEventBlock = self.MIDIOutputEventBlock;

  // Modern MIDI 2.0 / UMP output block + the host's desired output protocol
  // (macOS 12 / iOS 15+). Preferred over the legacy block when present.
  if (__builtin_available(macOS 12.0, iOS 15.0, *))
  {
    _impl->_processAdapter->midiOutputEventListBlock = self.MIDIOutputEventListBlock;
    _impl->_processAdapter->hostMIDIProtocol = self.hostMIDIProtocol;
  }

  // Publish the adapter for the render block and flip the flag under the
  // cache mutex BEFORE the plugin may start processing: producers
  // (implementorValueObserver, bypass setter) must switch from the flush
  // path to the render queue path first — flush during processing violates
  // the CLAP contract. Queued changes wait in the adapter until the first
  // render cycle. The mutex pairs with the producers' flag check.
  _impl->_processAdapterLive.store(_impl->_processAdapter.get());
  {
    std::lock_guard<std::mutex> lock(_impl->_paramCacheMutex);
    _renderResourcesAllocated = YES;
  }

  // Activate the CLAP plugin
  AUV3LOG("allocateRenderResources: calling activate()");
  _impl->_plugin->activate();

  // Re-cache latency — the plugin may have set it during activation
  if (_impl->_plugin->_ext._latency)
  {
    uint32_t newLatency = _impl->_plugin->_ext._latency->get(_impl->_plugin->_plugin);
    if (newLatency != _impl->_cachedLatencySamples)
    {
      _impl->_cachedLatencySamples = newLatency;
      AUV3LOG("allocateRenderResources: latency updated to %u samples after activate", newLatency);
      [self willChangeValueForKey:@"latency"];
      [self didChangeValueForKey:@"latency"];
    }
  }

  AUV3LOG("allocateRenderResources: calling start_processing()");
  _impl->_plugin->start_processing();
  _impl->_initialized = true;

  AUV3LOG("allocateRenderResources: completed successfully");
  return YES;
}

- (void)deallocateRenderResources
{
  AUV3LOG("deallocateRenderResources: entered (thread=%{public}s)",
          [NSThread.currentThread.name UTF8String] ?: "unnamed");

  // Unpublish the adapter, then wait for any in-flight render callback to
  // leave before stopping the plugin and freeing the adapter. The wait is
  // bounded by one render quantum.
  if (_impl)
  {
    _impl->_processAdapterLive.store(nullptr);
    while (_impl->_renderInFlight.load() != 0)
    {
      usleep(100);
    }
  }

  if (_impl && _impl->_plugin && _impl->_initialized)
  {
    auto guarantee_mainthread = _impl->_plugin->AlwaysMainThread();
    AUV3LOG("deallocateRenderResources: calling stop_processing()");
    _impl->_plugin->stop_processing();
    AUV3LOG("deallocateRenderResources: calling deactivate()");
    _impl->_plugin->deactivate();
    _impl->_initialized = false;
  }

  if (_impl)
  {
    // Producers (implementorValueObserver, bypass setter) check this flag
    // and touch the adapter under _paramCacheMutex — flip it under the
    // same mutex so a producer can never race the adapter reset below.
    // (The _processAdapterLive handshake above only covers the render
    // thread.)
    {
      std::lock_guard<std::mutex> lock(_impl->_paramCacheMutex);
      _renderResourcesAllocated = NO;
    }

    // Changes parked in the render queue while the last cycles ran would
    // otherwise be lost (the cache and host UI already show them) —
    // deliver them via flush, which is legal now that processing stopped.
    if (_impl->_processAdapter)
    {
      Clap::AUv3::ProcessAdapter::QueuedParamChange qpc;
      while (_impl->_processAdapter->dequeueParameterChange(qpc))
      {
        _impl->flushParamValue(qpc.id, qpc.value);
      }
    }
  }

  AUV3LOG("deallocateRenderResources: resetting process adapter");
  _impl->_processAdapter.reset();

  AUV3LOG("deallocateRenderResources: calling [super deallocateRenderResources]");
  [super deallocateRenderResources];
  AUV3LOG("deallocateRenderResources: completed");
}

// --- Render block ---

- (AUInternalRenderBlock)internalRenderBlock
{
  // Capture the stable _impl pointer — the framework may cache this block before
  // allocateRenderResources is called, so we must dereference _processAdapter at
  // render time rather than at block-creation time.
  auto *impl = _impl.get();

  return ^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags, const AudioTimeStamp *timestamp,
                            AUAudioFrameCount frameCount, NSInteger outputBusNumber,
                            AudioBufferList *outputData, const AURenderEvent *realtimeEventListHead,
                            AURenderPullInputBlock __unsafe_unretained pullInputBlock) {
    if (!impl) return kAudioUnitErr_Uninitialized;

    // Handshake with deallocateRenderResources: announce we're inside the
    // render block, THEN check the published adapter. Deallocation nulls
    // the pointer first and drains this counter before freeing, so either
    // we see null here or deallocation waits for us to finish.
    impl->_renderInFlight.fetch_add(1);
    auto *adapter = impl->_processAdapterLive.load();
    if (!adapter)
    {
      impl->_renderInFlight.fetch_sub(1);
      return kAudioUnitErr_Uninitialized;
    }

    // Force audio-thread identity for the duration of the render call.
    // In out-of-process AUv3, _main_thread_id was captured on the XPC worker
    // thread during init, so the default heuristic is wrong.
    AUAudioUnitStatus status = kAudioUnitErr_Uninitialized;
    {
      auto audioGuard = impl->_plugin->AlwaysAudioThread();
      status = adapter->process(actionFlags, timestamp, frameCount, outputBusNumber, outputData,
                                realtimeEventListHead, pullInputBlock);
    }
    impl->_renderInFlight.fetch_sub(1);

    // Do NOT dispatch on_main_thread() from the render block. Surge XT's
    // on_main_thread() acquires JUCE locks that process() also needs — dispatching
    // it asynchronously causes lock contention: on_main_thread() runs on main while
    // process() runs on render thread, both needing the same lock → deadlock.
    //
    // The _requestUICallback flag is still set by request_callback(). It will be
    // serviced when a GUI is active (via idle timer) or when the plugin is not
    // processing (e.g., after deallocateRenderResources).

    return status;
  };
}

// --- GUI methods for the view controller ---

- (BOOL)createGUIInView:(CLAPWRAP_ViewClass *)parentView
                  width:(uint32_t *)outWidth
                 height:(uint32_t *)outHeight
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;

  // In out-of-process AUv3, _main_thread_id was captured on the XPC worker
  // thread during init, so the CLAP proxy doesn't recognize the actual main
  // thread. Override the thread identity for all GUI calls.
  auto mainGuard = _impl->_plugin->AlwaysMainThread();

  auto *gui = _impl->_plugin->_ext._gui;
  auto *plugin = _impl->_plugin->_plugin;

  // CLAP defines _COCOA for NSView and no standard UIKit identifier. We use
  // a private _UIKIT string (see auv3_platform.h) on iOS. Hosted plugins
  // using clap-wrapper on iOS must recognise the same string.
#if TARGET_OS_IPHONE
  const char *windowApi = CLAP_WINDOW_API_UIKIT;
#else
  const char *windowApi = CLAP_WINDOW_API_COCOA;
#endif

  if (!gui->is_api_supported(plugin, windowApi, false))
  {
    AUV3ERR("createGUIInView: plugin rejected api '%{public}s'", windowApi);
    return NO;
  }

  if (!gui->create(plugin, windowApi, false))
  {
    AUV3ERR("createGUIInView: gui->create failed for api '%{public}s'", windowApi);
    return NO;
  }

  gui->set_scale(plugin, 1.0);

  uint32_t w = 0, h = 0;
  gui->get_size(plugin, &w, &h);

  if (gui->can_resize(plugin))
  {
    gui->adjust_size(plugin, &w, &h);
  }

  // Confirm the size to the plugin (matches VST3/AUv2 pattern).
  gui->set_size(plugin, w, h);

  // Resize the parent view BEFORE set_parent() so the CLAP plugin's
  // subview is created inside a properly-sized container. Without this
  // the container is 0x0 and plugins that clip to parent bounds are invisible.
  [parentView setFrame:CGRectMake(0, 0, w, h)];

  clap_window_t window;
  window.api = windowApi;
#if TARGET_OS_IPHONE
  // CLAP's clap_window union has no UIKit-typed member. The `ptr` slot is
  // the generic escape hatch; the hosted plugin reads it as a UIView*.
  window.ptr = (__bridge void *)parentView;
#else
  window.cocoa = (__bridge void *)parentView;
#endif
  gui->set_parent(plugin, &window);
  gui->show(plugin);

  if (outWidth) *outWidth = w;
  if (outHeight) *outHeight = h;

  // Update the IHost gui_request_resize to notify the view controller
  _impl->_guiParentView = parentView;

  return YES;
}

- (void)destroyGUI
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return;

  auto mainGuard = _impl->_plugin->AlwaysMainThread();
  _impl->_plugin->_ext._gui->hide(_impl->_plugin->_plugin);
  _impl->_plugin->_ext._gui->destroy(_impl->_plugin->_plugin);
  _impl->_guiParentView = nil;
  _impl->_viewController = nil;
}

- (BOOL)canResizeGUI
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;
  auto mainGuard = _impl->_plugin->AlwaysMainThread();
  return _impl->_plugin->_ext._gui->can_resize(_impl->_plugin->_plugin) ? YES : NO;
}

- (BOOL)setGUISize:(uint32_t)width height:(uint32_t)height
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;
  auto mainGuard = _impl->_plugin->AlwaysMainThread();
  return _impl->_plugin->_ext._gui->set_size(_impl->_plugin->_plugin, width, height) ? YES : NO;
}

- (void)setViewController:(ClapAUv3ViewController *)vc
{
  if (_impl) _impl->_viewController = vc;
}

// Main-queue continuation of gui_request_resize (see AUv3ImplDetail).
- (void)_applyGUISizeWidth:(uint32_t)width height:(uint32_t)height
{
  if (!_impl) return;
  __strong ClapAUv3ViewController *vc = _impl->_viewController;
  if (vc)
  {
    // CGRectMake / CGSizeMake work identically on macOS and iOS;
    // NSMakeRect / NSMakeSize are AppKit-only.
    vc.view.frame = CGRectMake(0, 0, width, height);

    [vc willChangeValueForKey:@"preferredContentSize"];
    vc.preferredContentSize = CGSizeMake(width, height);
    [vc didChangeValueForKey:@"preferredContentSize"];
  }
}

// --- View controller ---
// Override requestViewControllerWithCompletionHandler: to return the factory VC.
// The default AUAudioUnit implementation returns nil. The extension infrastructure
// may handle this automatically in some contexts, but explicitly returning the VC
// ensures the host can always obtain it (both in-process and out-of-process).

- (void)requestViewControllerWithCompletionHandler:
    (void (^)(AUViewControllerBase *__nullable))completionHandler
{
  AUV3LOG("requestViewControllerWithCompletionHandler: called (factoryVC=%p)", _factoryViewController);
  completionHandler(_factoryViewController);
}

// Tell the host this AU has a custom view. Without this, some hosts
// (Logic Pro) may never offer the "Custom" view option.
- (BOOL)providesUserInterface
{
  return (_impl && _impl->_plugin && _impl->_plugin->_ext._gui) ? YES : NO;
}

// --- Bypass ---

- (BOOL)shouldBypassEffect
{
  if (!_impl || _impl->_bypassParamId == CLAP_INVALID_ID) return NO;

  std::lock_guard<std::mutex> lock(_impl->_paramCacheMutex);
  auto it = _impl->_paramValueCache.find(_impl->_bypassParamId);
  if (it != _impl->_paramValueCache.end()) return it->second >= 0.5;
  return NO;
}

- (void)setShouldBypassEffect:(BOOL)shouldBypassEffect
{
  if (!_impl || _impl->_bypassParamId == CLAP_INVALID_ID) return;

  double newValue = shouldBypassEffect ? 1.0 : 0.0;

  // Update cache (and fetch the cookie in the same lock scope). While
  // rendering, route the change through the render thread's queue —
  // flush() is forbidden while the plugin is processing. The rendering
  // decision is made INSIDE the lock (paired with allocate/deallocate
  // flipping the flag under the same mutex).
  void *cookie = nullptr;
  bool queuedForRender = false;
  {
    std::lock_guard<std::mutex> lock(_impl->_paramCacheMutex);
    _impl->_paramValueCache[_impl->_bypassParamId] = newValue;
    auto cookieIt = _impl->_paramCookieCache.find(_impl->_bypassParamId);
    if (cookieIt != _impl->_paramCookieCache.end()) cookie = cookieIt->second;

    if (_renderResourcesAllocated)
    {
      if (_impl->_processAdapter)
      {
        _impl->_processAdapter->queueParameterChange(_impl->_bypassParamId, newValue);
      }
      queuedForRender = true;
    }
  }

  // Push to the CLAP plugin via params->flush() (only legal while not processing)
  if (!queuedForRender)
  {
    _impl->flushParamValueWithCookie(_impl->_bypassParamId, newValue, cookie);
  }

  // Update the AUParameter in the tree so the UI stays in sync. The set
  // re-enters implementorValueObserver on this thread — suppress the echo.
  if (_impl->_parameterTree)
  {
    AUParameter *param =
        [_impl->_parameterTree parameterWithAddress:(AUParameterAddress)_impl->_bypassParamId];
    if (param)
    {
      s_suppressParamObserverEcho = true;
      [param setValue:(AUValue)newValue originator:_impl->_parameterObserverToken];
      s_suppressParamObserverEcho = false;
    }
  }
}

@end

// Forward-declare private method used by ClapAUv3ContainerView
@interface ClapAUv3ViewController ()
- (void)_viewDidMoveToWindow;
@end

// -----------------------------------------------------------------------
// ClapAUv3ContainerView — custom view that notifies the VC when it enters
// or leaves a window. AUViewController lifecycle methods (viewDidAppear
// etc.) are unreliable when the host doesn't manage the VC hierarchy
// properly. (view)didMoveToWindow always fires.
//
// macOS and iOS expose the same *semantic* hooks under different selectors:
//   macOS NSView:  -viewDidMoveToWindow / -viewDidMoveToSuperview
//   iOS  UIView:   -didMoveToWindow    / -didMoveToSuperview
// UIView has no `isFlipped` (top-left is always the origin on iOS).
// -----------------------------------------------------------------------

@interface ClapAUv3ContainerView : CLAPWRAP_ViewClass
@property(nonatomic, weak) ClapAUv3ViewController *viewController;
@end

@implementation ClapAUv3ContainerView

#if TARGET_OS_OSX
- (BOOL)isFlipped
{
  // Plugin GUIs expect (0,0) at top-left (flipped coordinate system).
  return YES;
}

- (void)viewDidMoveToWindow
{
  [super viewDidMoveToWindow];
  [self.viewController _viewDidMoveToWindow];
}

- (void)viewDidMoveToSuperview
{
  [super viewDidMoveToSuperview];
  // viewDidMoveToWindow only fires when the window changes. For LoadInProcess,
  // the system puts the view in the host's window during factory creation.
  // When the host later calls addSubview:, the window is the SAME, so
  // viewDidMoveToWindow doesn't fire. viewDidMoveToSuperview fires in both cases.
  if (self.superview && self.window)
  {
    [self.viewController _viewDidMoveToWindow];
  }
}
#else  // TARGET_OS_IPHONE
- (void)didMoveToWindow
{
  [super didMoveToWindow];
  [self.viewController _viewDidMoveToWindow];
}

- (void)didMoveToSuperview
{
  [super didMoveToSuperview];
  if (self.superview && self.window)
  {
    [self.viewController _viewDidMoveToWindow];
  }
}
#endif

@end

// -----------------------------------------------------------------------
// ClapAUv3ViewController implementation (also serves as AUAudioUnitFactory)
// -----------------------------------------------------------------------

@implementation ClapAUv3ViewController
{
  BOOL _guiCreated;
}

- (void)loadView
{
  // Custom container view that detects when the view enters a window via
  // (view)didMoveToWindow / (view)didMoveToSuperview. AUViewController
  // lifecycle methods only fire when the VC is in the VC hierarchy —
  // many hosts just call addSubview:.
  //
  // Start with a reasonable default size. The viewbridge rejects
  // zero-sized views. The real size is set in _createPluginGUI once the
  // CLAP plugin reports its preferred dimensions.
  CGSize initialSize = CGSizeMake(400, 500);
  ClapAUv3ContainerView *view = [[ClapAUv3ContainerView alloc]
      initWithFrame:CGRectMake(0, 0, initialSize.width, initialSize.height)];
  view.viewController = self;
  view.translatesAutoresizingMaskIntoConstraints = YES;
  [self setView:view];
  self.preferredContentSize = initialSize;
}

- (void)setAudioUnit:(ClapAUv3AudioUnit *)audioUnit
{
  _audioUnit = audioUnit;
  // Establish the back-reference so the AU can return us from
  // requestViewControllerWithCompletionHandler:
  if (audioUnit) audioUnit->_factoryViewController = self;
}

- (void)_createPluginGUI
{
  if (!self.audioUnit) return;

  // Establish the back-reference so gui_request_resize can reach this VC
  [self.audioUnit setViewController:self];

  uint32_t w = 0, h = 0;
  if ([self.audioUnit createGUIInView:self.view width:&w height:&h])
  {
    AUV3LOG("GUI created, size=%ux%u", w, h);
    if (w > 0 && h > 0)
    {
      // Explicit KVO notifications — required for the remote proxy to
      // forward preferredContentSize changes across the XPC boundary
      // to the host process.
      self.view.frame = CGRectMake(0, 0, w, h);
      [self willChangeValueForKey:@"preferredContentSize"];
      self.preferredContentSize = CGSizeMake(w, h);
      [self didChangeValueForKey:@"preferredContentSize"];
    }
  }
}

// Convergence point for GUI creation. Called from multiple triggers:
// - viewDidMoveToWindow / viewDidMoveToSuperview (in-process)
// - viewDidAppear (out-of-process)
// Creates the GUI once all preconditions are met. Guarded by _guiCreated.
- (void)_tryCreateGUI
{
  if (_guiCreated) return;
  if (!self.audioUnit) return;
  if (!self.isViewLoaded || !self.view.window) return;

  _guiCreated = YES;
  [self _createPluginGUI];
}

// Called by ClapAUv3ContainerView when the view enters or leaves a window.
- (void)_viewDidMoveToWindow
{
  if (self.view.window)
  {
    dispatch_async(dispatch_get_main_queue(), ^{
      [self _tryCreateGUI];
    });
  }
  else
  {
    if (_guiCreated)
    {
      _guiCreated = NO;
      [self.audioUnit destroyGUI];
    }
  }
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  // Try to create the GUI early so preferredContentSize is set BEFORE
  // the host reads it via requestViewControllerWithCompletionHandler:.
  // For out-of-process AUv3, the proxy doesn't forward property changes,
  // so the host only sees the value that was set at VC creation time.
  // If gui->create() blocks (JUCE plugins), viewDidAppear handles it later.
  [self _tryCreateGUI];
}

// Out-of-process: the system manages the VC lifecycle properly, so
// viewDidAppear fires when the host displays the view.
// In-process: (view)didMoveToSuperview on the container view handles it.
// NOTE: AUViewController is NSViewController on macOS (viewDidAppear with
// no argument) and UIViewController on iOS (viewDidAppear: takes animated).
#if TARGET_OS_OSX
- (void)viewDidAppear
{
  [super viewDidAppear];
  [self _tryCreateGUI];
}
#else
- (void)viewDidAppear:(BOOL)animated
{
  [super viewDidAppear:animated];
  [self _tryCreateGUI];
}
#endif

// Host-driven layout propagates to the CLAP plugin's subview. macOS
// NSViewController uses -viewDidLayout; UIViewController exposes the
// analogous -viewDidLayoutSubviews (no super arg differences).
#if TARGET_OS_OSX
- (void)viewDidLayout
#else
- (void)viewDidLayoutSubviews
#endif
{
#if TARGET_OS_OSX
  [super viewDidLayout];
#else
  [super viewDidLayoutSubviews];
#endif
  if (!_guiCreated) return;

  CGRect bounds = self.view.bounds;
  if (bounds.size.width > 0 && bounds.size.height > 0)
  {
    // Propagate host-initiated container resize to the CLAP plugin
    if ([self.audioUnit canResizeGUI])
    {
      [self.audioUnit setGUISize:(uint32_t)bounds.size.width height:(uint32_t)bounds.size.height];
    }

    // Ensure the CLAP plugin's subview fills the container
    for (CLAPWRAP_ViewClass *subview in self.view.subviews)
    {
      subview.frame = bounds;
    }
  }
}

#if TARGET_OS_OSX
- (void)viewDidDisappear
{
  if (_guiCreated)
  {
    _guiCreated = NO;
    [self.audioUnit destroyGUI];
  }
  [super viewDidDisappear];
}
#else
- (void)viewDidDisappear:(BOOL)animated
{
  if (_guiCreated)
  {
    _guiCreated = NO;
    [self.audioUnit destroyGUI];
  }
  [super viewDidDisappear:animated];
}
#endif

- (void)dealloc
{
  if (_guiCreated)
  {
    [self.audioUnit destroyGUI];
    _guiCreated = NO;
  }
}

// --- AUAudioUnitFactory ---
// Base implementation -- subclasses generated by build-helper override this.

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
  AUV3ERR("createAudioUnitWithComponentDescription: BASE class called — subclass should override");
  if (error)
    *error = [NSError
        errorWithDomain:@"ClapAUv3"
                   code:-100
               userInfo:@{NSLocalizedDescriptionKey : @"Base factory should not be called directly"}];
  return nil;
}

- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context
{
  AUV3LOG("beginRequestWithExtensionContext: entered (context=%p)", context);
  // MUST call super — AUViewController uses this to set up the view bridge
  // service. Without it the host never receives the view controller and
  // the plugin's custom UI cannot be displayed.
  [super beginRequestWithExtensionContext:context];
  AUV3LOG("beginRequestWithExtensionContext: leaving (context=%p)", context);
}

@end

// #pragma clang diagnostic pop
