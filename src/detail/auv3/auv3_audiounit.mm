#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wlanguage-extension-token"

#import "auv3_audiounit.h"
#include "auv3_parameters.h"
#include "auv3_channellayout.h"
#include "process.h"

#include "clap_proxy.h"
#include "detail/clap/fsutil.h"
#include "detail/os/osutil.h"
#include "detail/shared/fixedqueue.h"
#include "detail/shared/spinlock.h"
#include "detail/clap/automation.h"

#include <os/log.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <cstring>
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
- (void)_applyRenderMode;
- (BOOL)_applyBusFormatsToPlugin;
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

  // Audio bus info, one per CLAP audio port: what the port is right now
  struct BusInfo
  {
    uint32_t channelCount = 0;
    std::string name;
    bool isMain = false;

    // what the port says about its layout - a CLAP_PORT_SURROUND map backed by
    // clap.surround, or a CLAP_PORT_AMBISONIC configuration backed by clap.ambisonic
    std::vector<uint8_t> channelMap;
    bool hasAmbisonicConfig = false;
    clap_ambisonic_config_t ambisonicConfig{};

    // the bus format's layout: described by the port, or guessed from its count
    AVAudioChannelLayout *layout = nil;
  };
  std::vector<BusInfo> _inputBusInfos;
  std::vector<BusInfo> _outputBusInfos;

  // The layouts the plugin accepts through clap.configurable-audio-ports,
  // probed once at init while it is deactivated - the only source of alternate
  // layouts, as in the AUv2 wrapper. Each records the channel count it gives
  // every bus. Empty without the extension, which pins every bus to its port.
  struct ProbedLayout
  {
    uint32_t mainIn = 0;
    uint32_t mainOut = 0;
    std::vector<uint32_t> inputBusChannels;
    std::vector<uint32_t> outputBusChannels;
  };
  static constexpr uint32_t kMaxProbedChannels = 8;
  std::vector<ProbedLayout> _probedLayouts;

  // Every layout tag a bus takes in one of the probed layouts, per bus - what
  // the host offers the user (kAudioUnitProperty_SupportedChannelLayoutTags).
  std::vector<std::vector<AudioChannelLayoutTag>> _inputLayoutTags;
  std::vector<std::vector<AudioChannelLayoutTag>> _outputLayoutTags;

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
  std::string _underlying_hostname;
  std::atomic<bool> _initialized{false};
  std::atomic_bool _requestUICallback{false};
  // set by mark_dirty(), serviced by the idle timer
  std::atomic_bool _requestMarkDirty{false};
  dispatch_source_t _idleTimer = nullptr;

  // "there are parameter events to move": set by param_request_flush() for
  // the plugin's direction and by the parameter observer / bypass setter for
  // the host's, serviced by the idle timer. See serviceFlushRequest().
  std::atomic_bool _requestedFlush{false};
  // set by the render block, cleared by the idle timer: tells the tick whether
  // a render has carried the parameter traffic since it last looked.
  std::atomic_bool _renderedSinceIdle{false};
  // consecutive idle ticks that saw no render, saturating at the point where
  // the idle tick takes over the flushing. Only the idle timer touches it.
  uint32_t _idleTicksSinceRender = 0;
  // how many of those ticks make a paused host, sized from the block period
  // in allocateRenderResourcesAndReturnError:. See serviceFlushRequest().
  static constexpr double kIdleTickMs = 10.0;
  static constexpr uint32_t kMinIdleTicksBeforeFlush = 5;
  std::atomic<uint32_t> _idleTicksBeforeFlush{kMinIdleTicksBeforeFlush};

  // The one lock that keeps clap_plugin_params.flush() and clap_plugin.process()
  // apart, which CLAP requires (clap/ext/params.h: flush "must not be called
  // concurrently to clap_plugin->process()"). Held by the render block around
  // process(), by every flush this wrapper issues, and by allocate/deallocate
  // around the activate/start_processing and stop_processing/deactivate
  // pairs so that, under it, _initialized is exactly "the plugin is active" —
  // which is what decides whether a flush must claim the audio thread or the
  // main thread. A spin lock because the render thread must never block on
  // a kernel primitive; the idle flush only runs once the host has stopped
  // rendering, so the render side effectively never finds it taken.
  ClapWrapper::detail::shared::SpinLock _processLock;

  // Back-reference to the ObjC audio unit (weak to avoid retain cycle)
  __weak ClapAUv3AudioUnit *_audioUnit = nil;

  // The native view (NSView on macOS, UIView on iOS) that the CLAP GUI
  // is parented to. Set by parentGUIInView:.
  __weak CLAPWRAP_ViewClass *_guiParentView = nil;

  // CLAP GUI state. The GUI is created (gui->create) as soon as anything needs
  // the plugin's size — AUv3 asks for it before the view has a window — and
  // parented (gui->set_parent) only once the view actually enters one.
  bool _guiCreated = false;
  bool _guiShown = false;
  uint32_t _guiWidth = 0;
  uint32_t _guiHeight = 0;

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
    // AUAudioUnit has no dirty flag. The nearest equivalent is a KVO
    // notification on fullState, which AUAudioUnit.h documents as bridged to
    // the v2 property kAudioUnitProperty_ClassInfo. Deferred to the idle timer
    // so that a host reacting to it by reading the state cannot re-enter the
    // plugin from inside the call that reported dirty, and so bursts collapse
    // into one notification per tick.
    _requestMarkDirty = true;
  }
  void restartPlugin() override
  {
    AUV3LOG("IHost::restartPlugin() called");
  }

  void request_process() override
  {
    // The AUv3 host owns the render resources: start_processing() happens in
    // allocateRenderResourcesAndReturnError:, which only the host calls. There
    // is nothing to wake from this side — but whatever parameter traffic the
    // request was really about can still be moved by a flush, so ask for one.
    AUV3LOG("IHost::request_process() called");
    _requestedFlush = true;
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
      // Move parameter events in both directions when the host has stopped
      // rendering. Runs first so that what the flush pulls out of the plugin
      // reaches the host in this tick's drain rather than the next.
      self->serviceFlushRequest();

      // Drain the parameter automation queue (Touch/Value/Release → host).
      // This is safe even while processing — it only touches AUParameter
      // objects on the main queue, no CLAP plugin calls.
      self->drainParameterQueue();

      // Fire CLAP timers — safe while processing since timer callbacks
      // run on the main thread, not the audio thread.
      self->fireTimers();

      // Tell the host that the state it last read is stale. Serviced even
      // while processing: no CLAP plugin call is made here.
      if (self->_requestMarkDirty.exchange(false))
      {
        ClapAUv3AudioUnit *au = self->_audioUnit;
        if (au)
        {
          // Both keys cross the appex XPC boundary: a v3 host sees the KVO,
          // and a v2-API host receives the bridged kAudioUnitProperty_ClassInfo
          // / kAudioUnitProperty_ClassInfoFromDocument notification. Note that
          // AudioToolbox also emits allParameterValues alongside these keys, so
          // a host may re-read parameter values as a side effect.
          [au willChangeValueForKey:@"fullState"];
          [au didChangeValueForKey:@"fullState"];
          [au willChangeValueForKey:@"fullStateForDocument"];
          [au didChangeValueForKey:@"fullStateForDocument"];
        }
      }

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

  // What a port is, including its layout. A plugin that implements
  // clap.surround or clap.ambisonic states the layout itself - the only way the
  // host can learn which channel is the centre, the LFE or a height channel;
  // for any other port the layout is guessed from the channel count.
  BusInfo scanPort(const clap_plugin_t *plugin, bool isInput, uint32_t index,
                   const clap_audio_port_info_t &info) const
  {
    BusInfo bus;
    bus.channelCount = info.channel_count;
    bus.name = info.name;
    bus.isMain = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0;

    const char *type = info.port_type;
    if (type && info.channel_count > 0 && _plugin)
    {
      if (!strcmp(type, CLAP_PORT_SURROUND) && _plugin->_ext._surround)
      {
        std::vector<uint8_t> map(info.channel_count);
        auto mapped = _plugin->_ext._surround->get_channel_map(plugin, isInput, index, map.data(),
                                                               info.channel_count);
        if (mapped == info.channel_count)
        {
          bus.layout = Clap::AUv3Channels::layoutFromChannelMap(map.data(), mapped);
          bus.channelMap = std::move(map);
        }
      }
      else if (!strcmp(type, CLAP_PORT_AMBISONIC) && _plugin->_ext._ambisonic)
      {
        clap_ambisonic_config_t config{};
        if (_plugin->_ext._ambisonic->get_config(plugin, isInput, index, &config))
        {
          bus.hasAmbisonicConfig = true;
          bus.ambisonicConfig = config;
          bus.layout = Clap::AUv3Channels::layoutFromAmbisonicConfig(config, info.channel_count);
        }
      }
    }

    if (!bus.layout)
    {
      bus.layout = Clap::AUv3Channels::layoutFromChannelCount(info.channel_count);
    }
    return bus;
  }

  std::vector<BusInfo> scanPorts(const clap_plugin_t *plugin,
                                 const clap_plugin_audio_ports_t *audioports, bool isInput) const
  {
    std::vector<BusInfo> result;
    auto count = audioports->count(plugin, isInput);
    for (decltype(count) i = 0; i < count; ++i)
    {
      clap_audio_port_info_t info;
      if (audioports->get(plugin, i, isInput, &info))
      {
        result.push_back(scanPort(plugin, isInput, i, info));
      }
    }
    return result;
  }

  void setupAudioBusses(const clap_plugin_t *plugin,
                        const clap_plugin_audio_ports_t *audioports) override
  {
    _inputBusInfos = scanPorts(plugin, audioports, true);
    _outputBusInfos = scanPorts(plugin, audioports, false);
  }

  static void addLayoutTag(std::vector<std::vector<AudioChannelLayoutTag>> &tags, size_t bus,
                           AVAudioChannelLayout *layout)
  {
    if (bus >= tags.size()) tags.resize(bus + 1);
    if (!layout || layout.layoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) return;
    auto &list = tags[bus];
    if (std::find(list.begin(), list.end(), layout.layoutTag) == list.end())
    {
      list.push_back(layout.layoutTag);
    }
  }

  // Remembers a port layout of the plugin as a probed one.
  void recordProbedLayout(uint32_t mainIn, uint32_t mainOut, const std::vector<BusInfo> &inputs,
                          const std::vector<BusInfo> &outputs)
  {
    ProbedLayout probed;
    probed.mainIn = mainIn;
    probed.mainOut = mainOut;
    for (size_t i = 0; i < inputs.size(); ++i)
    {
      probed.inputBusChannels.push_back(inputs[i].channelCount);
      addLayoutTag(_inputLayoutTags, i, inputs[i].layout);
    }
    for (size_t i = 0; i < outputs.size(); ++i)
    {
      probed.outputBusChannels.push_back(outputs[i].channelCount);
      addLayoutTag(_outputLayoutTags, i, outputs[i].layout);
    }
    _probedLayouts.push_back(std::move(probed));
  }

  // The main-bus (in, out) pairs this audio unit advertises, in the order
  // channelCapabilities reports them. A pair outside this list is one the
  // plugin was never found to accept.
  std::vector<std::pair<uint32_t, uint32_t>> advertisedMainPairs() const
  {
    std::vector<std::pair<uint32_t, uint32_t>> pairs;
    auto add = [&pairs](uint32_t in, uint32_t out)
    {
      auto pair = std::make_pair(in, out);
      if (std::find(pairs.begin(), pairs.end(), pair) == pairs.end()) pairs.push_back(pair);
    };

    if (!_probedLayouts.empty())
    {
      for (const auto &probed : _probedLayouts) add(probed.mainIn, probed.mainOut);
      return pairs;
    }

    std::vector<uint32_t> inCounts, outCounts;
    for (const auto &bus : _inputBusInfos) inCounts.push_back(bus.channelCount);
    for (const auto &bus : _outputBusInfos) outCounts.push_back(bus.channelCount);
    if (inCounts.empty()) inCounts.push_back(0);
    if (outCounts.empty()) outCounts.push_back(0);
    for (uint32_t ic : inCounts)
      for (uint32_t oc : outCounts) add(ic, oc);
    return pairs;
  }

  // The index of the main bus in a scope, or -1 where the scope has none.
  ptrdiff_t mainBusIndex(bool isInput) const
  {
    const auto &infos = isInput ? _inputBusInfos : _outputBusInfos;
    for (size_t i = 0; i < infos.size(); ++i)
      if (infos[i].isMain) return static_cast<ptrdiff_t>(i);
    return -1;
  }

  // One request per port, as the AUv2 and VST3 wrappers send them: the main
  // ports get the given counts, the others keep theirs - or, when nonMain is
  // non-zero, move to that.
  std::vector<clap_audio_port_configuration_request_t> mainBusRequests(uint32_t mainIn, uint32_t mainOut,
                                                                       uint32_t nonMain) const
  {
    std::vector<clap_audio_port_configuration_request_t> requests;
    auto add = [&requests](bool isInput, size_t port, uint32_t channels)
    {
      clap_audio_port_configuration_request_t request{};
      request.is_input = isInput;
      request.port_index = static_cast<uint32_t>(port);
      request.channel_count = channels;
      request.port_type =
          (channels == 1) ? CLAP_PORT_MONO : ((channels == 2) ? CLAP_PORT_STEREO : nullptr);
      request.port_details = nullptr;
      requests.push_back(request);
    };
    for (size_t i = 0; i < _inputBusInfos.size(); ++i)
    {
      const auto &bus = _inputBusInfos[i];
      add(true, i, bus.isMain ? mainIn : (nonMain ? nonMain : bus.channelCount));
    }
    for (size_t i = 0; i < _outputBusInfos.size(); ++i)
    {
      const auto &bus = _outputBusInfos[i];
      add(false, i, bus.isMain ? mainOut : (nonMain ? nonMain : bus.channelCount));
    }
    return requests;
  }

  // The requests that put every port back to what the bus infos say it is,
  // with the layout the port described where describe is set.
  std::vector<clap_audio_port_configuration_request_t> restoreRequests(bool describe) const
  {
    std::vector<clap_audio_port_configuration_request_t> requests;
    auto add = [&requests, describe](bool isInput, size_t port, const BusInfo &bus)
    {
      clap_audio_port_configuration_request_t request{};
      request.is_input = isInput;
      request.port_index = static_cast<uint32_t>(port);
      request.channel_count = bus.channelCount;
      request.port_type = (bus.channelCount == 1)   ? CLAP_PORT_MONO
                          : (bus.channelCount == 2) ? CLAP_PORT_STEREO
                                                    : nullptr;
      request.port_details = nullptr;
      if (describe && !request.port_type)
      {
        if (!bus.channelMap.empty())
        {
          request.port_type = CLAP_PORT_SURROUND;
          request.port_details = bus.channelMap.data();
        }
        else if (bus.hasAmbisonicConfig)
        {
          request.port_type = CLAP_PORT_AMBISONIC;
          request.port_details = &bus.ambisonicConfig;
        }
      }
      requests.push_back(request);
    };
    for (size_t i = 0; i < _inputBusInfos.size(); ++i) add(true, i, _inputBusInfos[i]);
    for (size_t i = 0; i < _outputBusInfos.size(); ++i) add(false, i, _outputBusInfos[i]);
    return requests;
  }

  // Probes the main-bus layouts the plugin accepts, the way the AUv2 wrapper's
  // PostConstructor does (wrapasauv2.cpp has the reasoning behind each choice
  // made here). Called once at init, with the plugin deactivated - the state
  // can_apply_configuration and apply_configuration require.
  void probeChannelLayouts()
  {
    _probedLayouts.clear();
    _inputLayoutTags.clear();
    _outputLayoutTags.clear();

    // the layout as it is, whether or not anything else can be probed
    for (size_t i = 0; i < _inputBusInfos.size(); ++i)
      addLayoutTag(_inputLayoutTags, i, _inputBusInfos[i].layout);
    for (size_t i = 0; i < _outputBusInfos.size(); ++i)
      addLayoutTag(_outputLayoutTags, i, _outputBusInfos[i].layout);

    auto *cap = _plugin ? _plugin->_ext._configurable_audio_ports : nullptr;
    if (!cap || !_plugin->_ext._audioports || _outputBusInfos.empty()) return;

    const clap_plugin_t *plugin = _plugin->_plugin;
    auto *audioports = _plugin->_ext._audioports;

    bool hasMainIn = false, hasMainOut = false;
    uint32_t currentIn = 0, currentOut = 0;
    for (const auto &bus : _inputBusInfos)
      if (bus.isMain)
      {
        hasMainIn = true;
        currentIn = bus.channelCount;
        break;
      }
    for (const auto &bus : _outputBusInfos)
      if (bus.isMain)
      {
        hasMainOut = true;
        currentOut = bus.channelCount;
        break;
      }

    // Only the main ports are varied, and a channel capability pair can only
    // describe the main busses: a scope with ports but none marked main can be
    // neither, so nothing is probed and every bus stays pinned to its port.
    if ((!_inputBusInfos.empty() && !hasMainIn) || !hasMainOut)
    {
      AUV3LOG("probeChannelLayouts: a scope has ports but no main port, not probing");
      return;
    }

    recordProbedLayout(currentIn, currentOut, _inputBusInfos, _outputBusInfos);

    const uint32_t minIn = currentIn ? 1 : 0, maxIn = currentIn ? kMaxProbedChannels : 0;
    const uint32_t minOut = currentOut ? 1 : 0, maxOut = currentOut ? kMaxProbedChannels : 0;

    bool moved = false;
    for (uint32_t in = minIn; in <= maxIn; ++in)
    {
      for (uint32_t out = minOut; out <= maxOut; ++out)
      {
        if (in == currentIn && out == currentOut) continue;  // recorded above

        // Two shapes per candidate: the non-main ports left where they are,
        // and taken along with the main input - a host may ask for either.
        // Without inputs both are the same request.
        for (uint32_t nonMain : {uint32_t{0}, in})
        {
          auto requests = mainBusRequests(in, out, nonMain);
          const auto size = static_cast<uint32_t>(requests.size());
          if (cap->can_apply_configuration(plugin, requests.data(), size) &&
              cap->apply_configuration(plugin, requests.data(), size))
          {
            moved = true;
            recordProbedLayout(in, out, scanPorts(plugin, audioports, true),
                               scanPorts(plugin, audioports, false));
          }
          if (in == 0) break;
        }
      }
    }

    if (moved)
    {
      // Back to the layout the plugin came up with, described as it described
      // it - a bare channel count could let a 7.1 come back as some other 8
      // channel layout. Rescanned either way: the bus infos are the wrapper's
      // only record of the live layout and must not describe one the plugin
      // did not return to.
      auto restore = restoreRequests(true);
      bool restored = cap->apply_configuration(plugin, restore.data(), (uint32_t)restore.size());
      if (!restored)
      {
        restore = restoreRequests(false);
        restored = cap->apply_configuration(plugin, restore.data(), (uint32_t)restore.size());
      }
      if (!restored)
      {
        AUV3LOG("probeChannelLayouts: could not restore the %u/%u layout", currentIn, currentOut);
      }
      setupAudioBusses(plugin, audioports);
    }

    AUV3LOG("probeChannelLayouts: %zu channel layouts", _probedLayouts.size());
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
    // May arrive at any moment and in any state, active or not: it says the
    // plugin has parameter events (its editor moved a knob), not that it has
    // stopped processing. clap_host_params.request_flush() obliges the host
    // to schedule process() or flush(); the render does the former while
    // the host is rendering, and the idle timer does the latter once it has
    // seen that no render is coming (see serviceFlushRequest()).
    _requestedFlush = true;
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

  const char *wrapper_flavor() const override
  {
    return CLAP_WRAPPER_HOST_FLAVOR_AUV3;
  }
  const char *underlying_host_name() const override
  {
    return _underlying_hostname.empty() ? nullptr : _underlying_hostname.c_str();
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

  // Output sink for the flushes this wrapper issues outside the process
  // adapter. The plugin may answer a flush with parameter events of its own
  // (a value its editor changed, the gesture around it, a dependent
  // parameter it moved in response) — they go down the same path a render's
  // would, so the host sees them either way.
  static bool flushOutputTryPush(const clap_output_events_t *list, const clap_event_header_t *ev)
  {
    auto *self = static_cast<AUv3ImplDetail *>(list->ctx);
    if (ev->space_id != CLAP_CORE_EVENT_SPACE_ID) return true;
    switch (ev->type)
    {
      case CLAP_EVENT_PARAM_VALUE:
        if (ev->size >= sizeof(clap_event_param_value_t))
          self->onPerformEdit(reinterpret_cast<const clap_event_param_value_t *>(ev));
        break;
      case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        if (ev->size >= sizeof(clap_event_param_gesture_t))
          self->onBeginEdit(reinterpret_cast<const clap_event_param_gesture_t *>(ev)->param_id);
        break;
      case CLAP_EVENT_PARAM_GESTURE_END:
        if (ev->size >= sizeof(clap_event_param_gesture_t))
          self->onEndEdit(reinterpret_cast<const clap_event_param_gesture_t *>(ev)->param_id);
        break;
      default:
        break;
    }
    return true;
  }

  // Calls clap_plugin_params.flush() with the given input list, excluded from
  // process() by _processLock and on the thread identity CLAP demands for the
  // plugin's current state: [active ? audio-thread : main-thread]. Under the
  // lock _initialized is exactly "active" — allocate publishes it after
  // start_processing and deallocate clears it after deactivate, both while
  // holding the lock — so the decision cannot be stale.
  void flushEvents(const clap_input_events_t *in_events)
  {
    ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);
    flushEventsLocked(in_events);
  }

  // The body of flushEvents(); the caller holds _processLock.
  void flushEventsLocked(const clap_input_events_t *in_events)
  {
    if (!_plugin || !_plugin->_ext._params) return;

    clap_output_events_t out_events = {};
    out_events.ctx = this;
    out_events.try_push = flushOutputTryPush;

    if (_initialized)
    {
      auto audioGuard = _plugin->AlwaysAudioThread();
      _plugin->_ext._params->flush(_plugin->_plugin, in_events, &out_events);
    }
    else
    {
      auto mainGuard = _plugin->AlwaysMainThread();
      _plugin->_ext._params->flush(_plugin->_plugin, in_events, &out_events);
    }
  }

  // The parameter flush, run from the idle timer on the main queue.
  //
  // In CLAP a plugin can only push an output event — a value its own editor
  // changed, the gesture around it — from inside process() or flush(), and
  // the host's own parameter sets only reach the plugin the same way (the
  // observer queues them on the process adapter while render resources are
  // allocated). Both directions therefore stop dead whenever the host stops
  // rendering, and AUv3 hosts do stop without deallocating: Audio Hijack
  // keeps the resources of a switched-off block, REAPER leaves an idle track's
  // unit initialized, and Apple documents renderResourcesAllocated as
  // resource state only, never as a promise that render calls will follow.
  // So the flag cannot decide whether a render will deliver; only the absence
  // of renders can, and this is the one place that observes it.
  //
  // One empty tick is not proof the host has paused, and guessing wrong costs
  // something: the host schedules its automation through the render event
  // list, and a flush that beat the next render would deliver the queued
  // values out of order with it. How long to wait cannot be a constant — at
  // 4096 frames and 44.1kHz a block is 93ms, so a host rendering perfectly
  // normally leaves gaps longer than any small number of 10ms ticks.
  // allocateRenderResources sizes the wait at three blocks, which no
  // rendering host produces; once one really has paused, the counter stays
  // saturated and a request is served on the very next tick. With the plugin
  // deactivated there is no render to wait for, and none is waited for.
  void serviceFlushRequest()
  {
    if (!_plugin) return;

    if (_renderedSinceIdle.exchange(false))
    {
      _idleTicksSinceRender = 0;
    }
    else if (_idleTicksSinceRender < _idleTicksBeforeFlush)
    {
      ++_idleTicksSinceRender;
    }

    if (!_requestedFlush) return;
    if (_initialized && _idleTicksSinceRender < _idleTicksBeforeFlush) return;

    // Cleared before the work, not after: a request that arrives while the
    // plugin is inside flush() is about events this flush cannot have seen,
    // and has to survive into the next tick.
    _requestedFlush = false;

    ClapWrapper::detail::shared::SpinLockGuard processGuard(_processLock);
    if (_initialized && _processAdapter)
    {
      // Active: the queued host changes live in the adapter, and flush() is
      // [audio-thread]; the lock is what lets the main queue stand in for it.
      auto audioGuard = _plugin->AlwaysAudioThread();
      _processAdapter->flush();
    }
    else
    {
      // Deactivated: host changes were already flushed one by one as they
      // arrived (flushParamValueWithCookie), so only the plugin's direction
      // is owed — an empty input list gives it somewhere to push. flush() is
      // [main-thread] here, which this queue is.
      clap_input_events_t in_events = {};
      in_events.ctx = nullptr;
      in_events.size = [](const clap_input_events_t *) -> uint32_t { return 0; };
      in_events.get = [](const clap_input_events_t *, uint32_t) -> const clap_event_header_t *
      { return nullptr; };
      flushEventsLocked(&in_events);
    }
  }

  // Deliver a single parameter value to the plugin via params->flush(). The
  // path the observer and the bypass setter take while render resources are
  // not allocated: no render can carry the change, and a host that sets a
  // value and reads fullState straight after must find it in the plugin, so
  // it is not deferred to the idle tick. Excluded from process() by
  // flushEvents(), which also covers the narrow window in which allocation
  // completes between the observer's routing decision and this call.
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

    flushEvents(&in_events);
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

// Intentionally never destroyed - see the note in wrapasvst3_entry.cpp: at
// process exit the hosted .clap is finalized before this binary, so calling
// clap_entry.deinit() from a static destructor aborts the host.
static Clap::Library &_library = *new Clap::Library();

// The CLAP window api this platform embeds into. NSView is _COCOA, UIView is
// _UIKIT — a hosted plugin must support the UIKit api to show a UI on iOS.
#if TARGET_OS_IPHONE
static const char *const _windowApi = CLAP_WINDOW_API_UIKIT;
#else
static const char *const _windowApi = CLAP_WINDOW_API_COCOA;
#endif

// Private view controller method called from the audio unit below.
@interface ClapAUv3ViewController (SizePublishing)
- (void)_publishSizeWidth:(uint32_t)width height:(uint32_t)height;
@end

// AUAudioUnitBus declares supportedChannelLayoutTags readonly, so a bus that
// wants to advertise layouts has to carry them itself and override the getter.
@interface ClapAUv3AudioUnitBus : AUAudioUnitBus
@property(nonatomic, copy, nullable) NSArray<NSNumber *> *clapLayoutTags;
@end

@implementation ClapAUv3AudioUnitBus
- (NSArray<NSNumber *> *)supportedChannelLayoutTags
{
  return self.clapLayoutTags;
}
@end

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
    _impl->_underlying_hostname = os::getHostAppName();
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

    // Find the channel layouts the plugin accepts besides its own, while it is
    // still deactivated.
    {
      auto mainGuard = _impl->_plugin->AlwaysMainThread();
      _impl->probeChannelLayouts();
    }

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
  using BusInfo = free_audio::auv3_wrapper::AUv3ImplDetail::BusInfo;
  using LayoutTags = std::vector<std::vector<AudioChannelLayoutTag>>;

  // A bus with more than two channels has to carry its layout:
  // initStandardFormatWithSampleRate:channels: returns nil for it, and the bus
  // used to be dropped without a word.
  auto makeBusses = [](const std::vector<BusInfo> &infos, const LayoutTags &tags)
  {
    NSMutableArray<AUAudioUnitBus *> *busses = [NSMutableArray new];
    for (size_t i = 0; i < infos.size(); ++i)
    {
      const auto &busInfo = infos[i];
      AVAudioFormat *format = nil;
      if (busInfo.layout && busInfo.layout.channelCount == busInfo.channelCount)
      {
        format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
                                                           channelLayout:busInfo.layout];
      }
      if (!format && busInfo.channelCount <= 2)
      {
        format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
                                                                channels:busInfo.channelCount];
      }
      if (!format)
      {
        AUV3ERR("_buildBusArrays: no format for '%{public}s' with %u channels", busInfo.name.c_str(),
                busInfo.channelCount);
        continue;
      }

      NSError *error = nil;
      ClapAUv3AudioUnitBus *bus = [[ClapAUv3AudioUnitBus alloc] initWithFormat:format error:&error];
      if (bus)
      {
        bus.name = [NSString stringWithUTF8String:busInfo.name.c_str()];
        if (i < tags.size() && !tags[i].empty())
        {
          NSMutableArray<NSNumber *> *supported = [NSMutableArray new];
          for (auto tag : tags[i]) [supported addObject:@(tag)];
          bus.clapLayoutTags = supported;
        }
        [busses addObject:bus];
      }
    }
    return busses;
  };

  _inputBusArray = [[AUAudioUnitBusArray alloc]
      initWithAudioUnit:self
                busType:AUAudioUnitBusTypeInput
                 busses:makeBusses(_impl->_inputBusInfos, _impl->_inputLayoutTags)];
  _outputBusArray = [[AUAudioUnitBusArray alloc]
      initWithAudioUnit:self
                busType:AUAudioUnitBusTypeOutput
                 busses:makeBusses(_impl->_outputBusInfos, _impl->_outputLayoutTags)];
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
    // While render resources are allocated, queue the change for the render
    // thread: the adapter's event vectors are render-thread-owned, so the
    // SPSC queue (producers serialized by this mutex) is the delivery path
    // — it is drained as input events at the top of the next render cycle,
    // or by the idle timer's flush if no render comes. The routing decision
    // is made INSIDE the lock: allocate/deallocate flip
    // _renderResourcesAllocated under the same mutex, so we can never queue
    // on a freed adapter.
    //
    // _renderResourcesAllocated only chooses the route, never promises
    // delivery: Apple defines it as resource state, and Audio Hijack keeps a
    // switched-off block's resources allocated with no render running. The
    // flush request below is what guarantees the change arrives regardless.
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
    if (queuedForRender)
    {
      // Nothing may ever render this: on a paused host process() is not
      // coming, and then the idle timer is the only thing that will hand it
      // to the plugin (see serviceFlushRequest()).
      strongSelf->_impl->_requestedFlush = true;
      return;
    }

    // Deactivated: push directly to the CLAP plugin via flush.
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

  // The main-bus pairs the probe found - the plugin's own first - or, without
  // the extension, the pairs the CLAP audio ports give, matching AUv2's
  // SupportedNumChannels(). A scope with no ports reports 0 on that side.
  NSMutableArray<NSNumber *> *caps = [NSMutableArray new];
  for (const auto &pair : _impl->advertisedMainPairs())
  {
    [caps addObject:@(pair.first)];
    [caps addObject:@(pair.second)];
  }
  return caps;
}

// Whether a bus can take a channel count: its port's own, or the one any probed
// layout gives that bus. Per bus and not per main bus only, because a layout may
// move a sidechain along with the main input (see ValidFormat in the AUv2
// wrapper). The count the host settles on is applied to the plugin in
// allocateRenderResources, which refuses a combination no layout has.
- (BOOL)_bus:(NSUInteger)index isInput:(BOOL)isInput acceptsChannels:(uint32_t)channels
{
  const auto &infos = isInput ? _impl->_inputBusInfos : _impl->_outputBusInfos;
  if (index >= infos.size())
  {
    AUV3LOG("shouldChangeToFormat: %{public}s bus %lu out of range", isInput ? "input" : "output",
            (unsigned long)index);
    return NO;
  }

  BOOL ok = (channels == infos[index].channelCount);
  for (const auto &probed : _impl->_probedLayouts)
  {
    if (ok) break;
    const auto &counts = isInput ? probed.inputBusChannels : probed.outputBusChannels;
    ok = index < counts.size() && counts[index] == channels;
  }

  AUV3LOG("shouldChangeToFormat: %{public}s bus %lu requested %u ch, port has %u -> %{public}s",
          isInput ? "input" : "output", (unsigned long)index, channels, infos[index].channelCount,
          ok ? "YES" : "NO");
  return ok;
}

// Whether changing the main bus of one scope to a channel count leaves the unit
// on a pair channelCapabilities advertises, given what the other scope's main
// bus carries now. Only asked once the unit is initialized: before that a host
// is free to walk through pairs on its way to the one it wants, and
// allocateRenderResources has the plugin itself to answer to.
- (BOOL)_mainPairStaysValidFor:(NSUInteger)index isInput:(BOOL)isInput channels:(uint32_t)channels
{
  if (!self.renderResourcesAllocated) return YES;

  const ptrdiff_t main = _impl->mainBusIndex(isInput);
  if (main < 0 || static_cast<NSUInteger>(main) != index) return YES;

  const ptrdiff_t other = _impl->mainBusIndex(!isInput);
  AUAudioUnitBusArray *otherBusses = isInput ? self.outputBusses : self.inputBusses;
  uint32_t otherChannels = 0;
  if (other >= 0 && static_cast<NSUInteger>(other) < otherBusses.count)
    otherChannels = otherBusses[static_cast<NSUInteger>(other)].format.channelCount;

  const uint32_t in = isInput ? channels : otherChannels;
  const uint32_t out = isInput ? otherChannels : channels;

  for (const auto &pair : _impl->advertisedMainPairs())
    if (pair.first == in && pair.second == out) return YES;

  AUV3LOG("shouldChangeToFormat: %u/%u is not a channel capability of this unit", in, out);
  return NO;
}

- (BOOL)shouldChangeToFormat:(AVAudioFormat *)format forBus:(AUAudioUnitBus *)bus
{
  if (!_impl) return NO;

  uint32_t requestedChannels = format.channelCount;

  for (NSUInteger i = 0; i < self.inputBusses.count; ++i)
  {
    if (self.inputBusses[i] == bus)
      return [self _bus:i isInput:YES acceptsChannels:requestedChannels] &&
             [self _mainPairStaysValidFor:i isInput:YES channels:requestedChannels];
  }

  for (NSUInteger i = 0; i < self.outputBusses.count; ++i)
  {
    if (self.outputBusses[i] == bus)
      return [self _bus:i isInput:NO acceptsChannels:requestedChannels] &&
             [self _mainPairStaysValidFor:i isInput:NO channels:requestedChannels];
  }

  AUV3LOG("shouldChangeToFormat: bus not found, rejecting");
  return NO;
}

// Hands the formats the host settled on to the plugin, before it is activated:
// a channel count the plugin's port does not have yet, and the layout of a bus
// where the plugin can tell layouts apart. Mirrors setBusArrangements in the
// VST3 wrapper - described first, by channel count alone if the plugin turns
// the description down. Returns NO when the host's counts cannot be had, which
// the render path could not survive.
- (BOOL)_applyBusFormatsToPlugin
{
  auto *cap = _impl->_plugin->_ext._configurable_audio_ports;
  auto *audioports = _impl->_plugin->_ext._audioports;
  const clap_plugin_t *plugin = _impl->_plugin->_plugin;
  if (!cap || !audioports) return YES;

  const size_t numIn = std::min<size_t>(_impl->_inputBusInfos.size(), self.inputBusses.count);
  const size_t numOut = std::min<size_t>(_impl->_outputBusInfos.size(), self.outputBusses.count);
  const size_t numPorts = numIn + numOut;

  // the channel maps and ambisonic configurations outlive the requests pointing
  // at them, and are sized once so that filling them cannot move them
  std::vector<std::vector<uint8_t>> channelMaps(numPorts);
  std::vector<clap_ambisonic_config_t> ambisonicConfigs(numPorts);
  std::vector<clap_audio_port_configuration_request_t> requests, undescribed;

  bool countsDiffer = false, layoutsDiffer = false, describesLayout = false;

  for (size_t i = 0; i < numPorts; ++i)
  {
    const bool isInput = i < numIn;
    const size_t port = isInput ? i : i - numIn;
    const auto &info = isInput ? _impl->_inputBusInfos[port] : _impl->_outputBusInfos[port];
    AVAudioFormat *format = (isInput ? self.inputBusses : self.outputBusses)[port].format;
    const uint32_t channels = format.channelCount;

    clap_audio_port_configuration_request_t request{};
    request.is_input = isInput;
    request.port_index = static_cast<uint32_t>(port);
    request.channel_count = channels;
    request.port_type =
        (channels == 1) ? CLAP_PORT_MONO : ((channels == 2) ? CLAP_PORT_STEREO : nullptr);
    request.port_details = nullptr;
    undescribed.push_back(request);

    countsDiffer |= (channels != info.channelCount);

    const AudioChannelLayout *layout = format.channelLayout ? format.channelLayout.layout : nullptr;
    if (!request.port_type && layout)
    {
      if (Clap::AUv3Channels::ambisonicConfigFromLayout(layout, channels, ambisonicConfigs[i]))
      {
        request.port_type = CLAP_PORT_AMBISONIC;
        request.port_details = &ambisonicConfigs[i];
        describesLayout = true;
        // only a plugin that reports its configuration can be found to differ
        if (_impl->_plugin->_ext._ambisonic)
        {
          layoutsDiffer |= !info.hasAmbisonicConfig ||
                           info.ambisonicConfig.ordering != ambisonicConfigs[i].ordering ||
                           info.ambisonicConfig.normalization != ambisonicConfigs[i].normalization;
        }
      }
      else if (Clap::AUv3Channels::channelMapFromLayout(layout, channels, channelMaps[i]))
      {
        request.port_type = CLAP_PORT_SURROUND;
        request.port_details = channelMaps[i].data();
        describesLayout = true;
        if (_impl->_plugin->_ext._surround)
        {
          layoutsDiffer |= (info.channelMap != channelMaps[i]);
        }
      }
    }
    requests.push_back(request);
  }

  if (!countsDiffer && !layoutsDiffer) return YES;

  auto apply = [&](const std::vector<clap_audio_port_configuration_request_t> &set)
  { return cap->apply_configuration(plugin, set.data(), static_cast<uint32_t>(set.size())); };

  if (!(apply(requests) || (describesLayout && apply(undescribed))))
  {
    // Nothing is lost when only the layout was asked for: the counts the
    // plugin keeps are the ones the host set.
    AUV3LOG("_applyBusFormatsToPlugin: the plugin refused the bus formats");
    return !countsDiffer;
  }

  // The ports changed: rescan them - the plugin is deactivated, so that is
  // legal - and check they are what the render path is going to hand over.
  _impl->setupAudioBusses(plugin, audioports);
  for (size_t i = 0; i < numPorts; ++i)
  {
    const bool isInput = i < numIn;
    const size_t port = isInput ? i : i - numIn;
    const auto &infos = isInput ? _impl->_inputBusInfos : _impl->_outputBusInfos;
    const uint32_t channels = (isInput ? self.inputBusses : self.outputBusses)[port].format.channelCount;
    if (port >= infos.size() || infos[port].channelCount != channels)
    {
      AUV3ERR("_applyBusFormatsToPlugin: %{public}s port %zu does not have the %u channels "
              "the plugin accepted",
              isInput ? "input" : "output", port, channels);
      return NO;
    }
  }

  AUV3LOG("_applyBusFormatsToPlugin: applied the host's bus formats");
  return YES;
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

  // The busses have to match the plugin's ports before anything is set up
  // from them.
  if (![self _applyBusFormatsToPlugin])
  {
    if (outError)
      *outError = [NSError errorWithDomain:NSOSStatusErrorDomain
                                      code:kAudioUnitErr_FormatNotSupported
                                  userInfo:@{
                                    NSLocalizedDescriptionKey :
                                        @"The CLAP plugin does not support this channel configuration"
                                  }];
    [super deallocateRenderResources];
    return NO;
  }

  AUV3LOG("allocateRenderResources: setting sample rate and block sizes");
  _impl->_plugin->setSampleRate(sampleRate);
  _impl->_plugin->setBlockSizes(1, self.maximumFramesToRender);

  // The mode is set whenever the host changes it; once more before activation
  // for a plugin that only looks at it there.
  [self _applyRenderMode];

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

  // How long the idle timer waits before it decides the host has paused the
  // render. A fixed number of ticks cannot work: at 4096 frames and 44.1kHz a
  // block is 93ms, so a host that is rendering perfectly normally leaves gaps
  // longer than any small constant, and the idle tick would flush inside
  // every one of them. Three blocks is a gap no rendering host produces.
  {
    const double blockMs = 1000.0 * (double)self.maximumFramesToRender / std::max(sampleRate, 1.0);
    const auto ticks = (uint32_t)std::ceil(3.0 * blockMs / _impl->kIdleTickMs);
    _impl->_idleTicksBeforeFlush = std::max(_impl->kMinIdleTicksBeforeFlush, ticks);
  }

  // Set when the plugin refuses to come up, and acted on below once the lock
  // this block takes has been dropped again.
  BOOL clapStartFailed = NO;

  {
    // Under _processLock from here to _initialized: no flush can slip in
    // between activate() and start_processing() and claim the wrong thread
    // for an already-active plugin, and no render can reach process() before
    // start_processing() (the adapter is published before activation).
    ClapWrapper::detail::shared::SpinLockGuard processGuard(_impl->_processLock);

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
    const bool clapActivated = _impl->_plugin->activate();

    // Both returns have to be honoured, and they have to be honoured together.
    // CLAP forbids process() on a plugin whose start_processing() returned
    // false, and forbids the matching stop_processing() as well; a plugin that
    // never activated must not be asked to start in the first place, nor
    // deactivated afterwards. There is no half-allocated state to park the
    // unit in — _initialized is what tells the idle flush whether the plugin
    // is active — so undo the whole allocation and fail the host's call.
    AUV3LOG("allocateRenderResources: calling start_processing()");
    if (!clapActivated || !_impl->_plugin->start_processing())
    {
      AUV3ERR("allocateRenderResources: %{public}s failed",
              clapActivated ? "start_processing()" : "activate()");

      if (clapActivated)
      {
        AUV3LOG("allocateRenderResources: calling deactivate()");
        _impl->_plugin->deactivate();
      }

      // Unwind in the reverse order of the setup above. Nothing has rendered
      // and nothing can have been queued for a render — producers check
      // _renderResourcesAllocated under _paramCacheMutex, which is cleared
      // here — so the adapter holds no events anyone is still owed.
      _impl->_processAdapterLive.store(nullptr);
      {
        std::lock_guard<std::mutex> lock(_impl->_paramCacheMutex);
        _renderResourcesAllocated = NO;
      }
      _impl->_processAdapter.reset();

      clapStartFailed = YES;
    }
    else
    {
      _impl->_initialized = true;
    }
  }

  if (clapStartFailed)
  {
    if (outError)
      *outError = [NSError
          errorWithDomain:NSOSStatusErrorDomain
                     code:kAudioUnitErr_FailedInitialization
                 userInfo:@{NSLocalizedDescriptionKey : @"The CLAP plugin refused to start processing"}];
    // Tell the base class the allocation did not stand, so the host does not
    // see an audio unit that reports allocated render resources it cannot use.
    [super deallocateRenderResources];
    return NO;
  }

  // Re-cache latency — the plugin may have set it during activation. Outside
  // the lock: the KVO runs the host's listeners synchronously.
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

  if (_impl)
  {
    // Under _processLock from stop_processing() to the adapter reset: the
    // idle timer's flush checks _initialized and _processAdapter under the
    // same lock, so it can neither claim the audio thread for a plugin that
    // is being deactivated nor flush through an adapter that is being freed.
    // Taken only after the render handshake above completed, so a render
    // that holds the lock is never waited for while _renderInFlight is held.
    ClapWrapper::detail::shared::SpinLockGuard processGuard(_impl->_processLock);

    if (_impl->_plugin && _impl->_initialized)
    {
      auto guarantee_mainthread = _impl->_plugin->AlwaysMainThread();
      AUV3LOG("deallocateRenderResources: calling stop_processing()");
      _impl->_plugin->stop_processing();
      AUV3LOG("deallocateRenderResources: calling deactivate()");
      _impl->_plugin->deactivate();
      _impl->_initialized = false;
    }

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
    // otherwise be lost (the cache and host UI already show them) — deliver
    // them via flush, which is [main-thread] now that the plugin is
    // deactivated, and hand the host whatever the plugin still had to say.
    if (_impl->_processAdapter && _impl->_plugin)
    {
      auto guarantee_mainthread = _impl->_plugin->AlwaysMainThread();
      _impl->_processAdapter->flush();
    }

    AUV3LOG("deallocateRenderResources: resetting process adapter");
    _impl->_processAdapter.reset();
  }

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
      // Excludes the idle timer's flush (see serviceFlushRequest()), which
      // only runs once three blocks have passed without a render — so in a
      // rendering host this lock is free. Taken after the _renderInFlight
      // announcement so deallocation, which waits on that counter without
      // holding the lock, cannot form a cycle with a render waiting here.
      ClapWrapper::detail::shared::SpinLockGuard processGuard(impl->_processLock);
      auto audioGuard = impl->_plugin->AlwaysAudioThread();
      status = adapter->process(actionFlags, timestamp, frameCount, outputBusNumber, outputData,
                                realtimeEventListHead, pullInputBlock);
    }
    // This render carried whatever was queued in either direction, so the
    // next idle tick has nothing to make up for.
    impl->_renderedSinceIdle = true;
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

- (BOOL)prepareGUIAndReturnWidth:(uint32_t *)outWidth height:(uint32_t *)outHeight
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;

  if (_impl->_guiCreated)
  {
    if (outWidth) *outWidth = _impl->_guiWidth;
    if (outHeight) *outHeight = _impl->_guiHeight;
    return YES;
  }

  // In out-of-process AUv3, _main_thread_id was captured on the XPC worker
  // thread during init, so the CLAP proxy doesn't recognize the actual main
  // thread. Override the thread identity for all GUI calls.
  auto mainGuard = _impl->_plugin->AlwaysMainThread();

  auto *gui = _impl->_plugin->_ext._gui;
  auto *plugin = _impl->_plugin->_plugin;

  if (!gui->is_api_supported(plugin, _windowApi, false))
  {
    AUV3ERR("prepareGUI: plugin rejected api '%{public}s'", _windowApi);
    return NO;
  }

  if (!gui->create(plugin, _windowApi, false))
  {
    AUV3ERR("prepareGUI: gui->create failed for api '%{public}s'", _windowApi);
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

  _impl->_guiCreated = true;
  _impl->_guiWidth = w;
  _impl->_guiHeight = h;

  if (outWidth) *outWidth = w;
  if (outHeight) *outHeight = h;

  return YES;
}

- (BOOL)parentGUIInView:(CLAPWRAP_ViewClass *)parentView
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;
  if (!_impl->_guiCreated) return NO;

  auto mainGuard = _impl->_plugin->AlwaysMainThread();

  auto *gui = _impl->_plugin->_ext._gui;
  auto *plugin = _impl->_plugin->_plugin;

  // Resize the parent view BEFORE set_parent() so the CLAP plugin's
  // subview is created inside a properly-sized container. Without this
  // the container is 0x0 and plugins that clip to parent bounds are invisible.
  if (_impl->_guiWidth > 0 && _impl->_guiHeight > 0)
  {
    [parentView setFrame:CGRectMake(0, 0, _impl->_guiWidth, _impl->_guiHeight)];
  }

  clap_window_t window;
  window.api = _windowApi;
#if TARGET_OS_IPHONE
  // CLAP's clap_window union has no UIKit-typed member. The `ptr` slot is
  // the generic escape hatch; the hosted plugin reads it as a UIView*.
  window.ptr = (__bridge void *)parentView;
#else
  window.cocoa = (__bridge void *)parentView;
#endif
  gui->set_parent(plugin, &window);
  gui->show(plugin);
  _impl->_guiShown = true;

  // Update the IHost gui_request_resize to notify the view controller
  _impl->_guiParentView = parentView;

  return YES;
}

- (void)destroyGUI
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return;
  if (!_impl->_guiCreated) return;

  auto mainGuard = _impl->_plugin->AlwaysMainThread();
  // Only hide what was shown: the GUI may have been created purely to answer a
  // size query and never parented.
  if (_impl->_guiShown) _impl->_plugin->_ext._gui->hide(_impl->_plugin->_plugin);
  _impl->_plugin->_ext._gui->destroy(_impl->_plugin->_plugin);
  _impl->_guiCreated = false;
  _impl->_guiShown = false;
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

  // CLAP contract: a host must pass set_size a size the plugin agreed to.
  // Container bounds are arbitrary (rotation, split view), so run them
  // through adjust_size first — a fixed-aspect plugin snaps them here, and
  // strict clap-helpers builds treat an unadjusted set_size as host
  // misbehaviour.
  auto *gui = _impl->_plugin->_ext._gui;
  if (gui->adjust_size) gui->adjust_size(_impl->_plugin->_plugin, &width, &height);
  if (!gui->set_size(_impl->_plugin->_plugin, width, height)) return NO;

  // Keep the cached size in step: prepareGUIAndReturnWidth:height: hands it
  // back, and that is how the view controller learns the adjusted size.
  _impl->_guiWidth = width;
  _impl->_guiHeight = height;
  return YES;
}

- (void)setViewController:(ClapAUv3ViewController *)vc
{
  if (_impl) _impl->_viewController = vc;
}

// Main-queue continuation of gui_request_resize (see AUv3ImplDetail).
- (void)_applyGUISizeWidth:(uint32_t)width height:(uint32_t)height
{
  if (!_impl) return;
  _impl->_guiWidth = width;
  _impl->_guiHeight = height;
  __strong ClapAUv3ViewController *vc = _impl->_viewController;
  if (vc) [vc _publishSizeWidth:width height:height];
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

  // Update cache (and fetch the cookie in the same lock scope). While render
  // resources are allocated, route the change through the render thread's
  // queue, same as implementorValueObserver — and ask for a flush, since a
  // paused host will never render it. The routing decision is made INSIDE
  // the lock (paired with allocate/deallocate flipping the flag under the
  // same mutex).
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

  if (queuedForRender)
  {
    _impl->_requestedFlush = true;
  }
  else
  {
    // Deactivated: push to the CLAP plugin via params->flush() directly.
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

// --- Offline rendering ---

// A host that bounces faster than real time says so here - Logic sets it for
// an offline bounce, it is kAudioUnitProperty_OfflineRender on the AUv2 side of
// the bridge. Passed on through clap.render, as the VST3 wrapper does for
// kOffline, so a plugin can switch to its offline quality or drop a bypass it
// only uses live.
- (void)setRenderingOffline:(BOOL)renderingOffline
{
  [super setRenderingOffline:renderingOffline];
  AUV3LOG("setRenderingOffline: %{public}s", renderingOffline ? "YES" : "NO");
  [self _applyRenderMode];
}

- (void)_applyRenderMode
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._render) return;

  auto *render = _impl->_plugin->_ext._render;
  const clap_plugin_t *plugin = _impl->_plugin->_plugin;

  // render.set is [main-thread], and a host sets the property from wherever
  // it happens to be - the same reasoning as every other host-driven call here.
  auto mainGuard = _impl->_plugin->AlwaysMainThread();

  clap_plugin_render_mode mode = self.renderingOffline ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME;
  if (mode == CLAP_RENDER_OFFLINE && render->has_hard_realtime_requirement(plugin))
  {
    // the property cannot be refused, so the plugin simply stays in real time
    AUV3LOG("_applyRenderMode: plugin has a hard real-time requirement, staying in real time");
    mode = CLAP_RENDER_REALTIME;
  }

  if (!render->set(plugin, mode))
  {
    AUV3LOG("_applyRenderMode: plugin refused render mode %d", (int)mode);
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

// The viewbridge rejects zero-sized views, so the view is born with this
// placeholder. It only ever reaches a host that has no plugin GUI to ask about.
static const CGSize kClapAUv3PlaceholderSize = {400, 500};

@implementation ClapAUv3ViewController
{
  // The CLAP GUI exists (gui->create succeeded). Independent of _guiParented:
  // AUv3 wants the plugin's size before the view has a window, so the GUI is
  // created as soon as the size is needed and parented later.
  BOOL _guiPrepared;
  // The CLAP GUI is embedded in our view and shown (set_parent + show).
  BOOL _guiParented;
  // Re-entrancy guard: publishing preferredContentSize can come back into the
  // getter through KVO observers.
  BOOL _preparing;
  // _pluginSize holds the plugin's own size. AppKit's stored property is only
  // a mirror of it: -setView: overwrites the property with the view's frame, so
  // the ivar is the source of truth the getter answers from.
  BOOL _sizeKnown;
  CGSize _pluginSize;
  // We already tried to create the audio unit ourselves (see _bootstrapAudioUnit).
  BOOL _bootstrapAttempted;
  // Serializes the two creation paths (see -createAudioUnitOnce:error:).
  std::mutex _audioUnitMutex;
}

- (void)loadView
{
  // Ask the plugin before the view exists. The host reads preferredContentSize
  // as soon as the view controller is vended — before the view has a window —
  // and the viewbridge doesn't forward later changes; -setView: also overwrites
  // preferredContentSize with the view's frame. So the view has to be born at
  // the right size. Create the audio unit ourselves if the host hasn't got
  // round to the factory method yet.
  [self _bootstrapAudioUnit];
  [self _ensureGUIPrepared];

  // Custom container view that detects when the view enters a window via
  // (view)didMoveToWindow / (view)didMoveToSuperview. AUViewController
  // lifecycle methods only fire when the VC is in the VC hierarchy —
  // many hosts just call addSubview:. The viewbridge rejects zero-sized views,
  // hence the placeholder when the plugin has no GUI to ask about.
  CGSize initialSize = _sizeKnown ? _pluginSize : kClapAUv3PlaceholderSize;
  ClapAUv3ContainerView *view = [[ClapAUv3ContainerView alloc]
      initWithFrame:CGRectMake(0, 0, initialSize.width, initialSize.height)];
  view.viewController = self;
  view.translatesAutoresizingMaskIntoConstraints = YES;
  [self setView:view];
  self.preferredContentSize = initialSize;

  AUV3LOG("loadView: view %dx%d (plugin size known=%d)", (int)initialSize.width, (int)initialSize.height,
          _sizeKnown ? 1 : 0);
}

- (void)setAudioUnit:(ClapAUv3AudioUnit *)audioUnit
{
  if (_audioUnit == audioUnit) return;
  // Never drop a live audio unit by bare overwrite: releasing the last reference
  // reaches plugin->terminate() with the GUI still created, which CLAP forbids.
  if (_audioUnit) [self _destroyGUI];
  _audioUnit = audioUnit;
  // Establish the back-reference so the AU can return us from
  // requestViewControllerWithCompletionHandler:
  if (audioUnit) audioUnit->_factoryViewController = self;

  // The viewbridge can load the view before the factory method runs, in which
  // case loadView had no plugin to ask. Now there is one. Out-of-process this
  // runs on an XPC worker thread, and everything below — view access included —
  // is main thread only.
  if (audioUnit)
  {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (self.isViewLoaded) [self _ensureGUIPrepared];
    });
  }
}

// A 4-character-code Info.plist string ("aufx") as an OSType.
static BOOL clapAUv3FourCC(id value, OSType *outCode)
{
  if (![value isKindOfClass:NSString.class]) return NO;
  const char *chars = [(NSString *)value UTF8String];
  if (!chars || strlen(chars) != 4) return NO;
  *outCode = ((OSType)(uint8_t)chars[0] << 24) | ((OSType)(uint8_t)chars[1] << 16) |
             ((OSType)(uint8_t)chars[2] << 8) | (OSType)(uint8_t)chars[3];
  return YES;
}

// Our own entry in the appex's AudioComponents array — the one whose
// factoryFunction names this class.
- (BOOL)_ownComponentDescription:(AudioComponentDescription *)outDesc
{
  NSDictionary *extension =
      [[NSBundle bundleForClass:[self class]] objectForInfoDictionaryKey:@"NSExtension"];
  NSArray *components = extension[@"NSExtensionAttributes"][@"AudioComponents"];
  NSString *className = NSStringFromClass([self class]);

  for (id entry in components)
  {
    if (![entry isKindOfClass:NSDictionary.class]) continue;
    NSDictionary *component = entry;
    if (![className isEqualToString:component[@"factoryFunction"]]) continue;

    AudioComponentDescription desc = {};
    if (!clapAUv3FourCC(component[@"type"], &desc.componentType)) return NO;
    if (!clapAUv3FourCC(component[@"subtype"], &desc.componentSubType)) return NO;
    if (!clapAUv3FourCC(component[@"manufacturer"], &desc.componentManufacturer)) return NO;
    *outDesc = desc;
    return YES;
  }
  return NO;
}

// Both creation paths meet here: the host's factory call, which arrives on an
// XPC worker thread, and -loadView's self-bootstrap on the appex main thread.
// Unserialized they both see a nil audioUnit and build an audio unit, and the
// loser is then thrown away with its GUI still alive. Nothing under this lock
// ever waits on the main queue, so the losing path only stalls.
- (AUAudioUnit *)createAudioUnitOnce:(ClapAUv3AudioUnit * (^)(NSError **))make error:(NSError **)error
{
  std::lock_guard<std::mutex> lock(_audioUnitMutex);
  if (_audioUnit) return _audioUnit;

  ClapAUv3AudioUnit *au = make(error);
  if (!au) return nil;
  self.audioUnit = au;
  return au;
}

// Out-of-process AUv3: the viewbridge loads our view from inside
// -beginRequestWithExtensionContext:, before the host has asked the factory for
// an audio unit, and it captures preferredContentSize as the view is vended —
// nothing published afterwards reaches the host process. So the plugin has to
// exist by then. Create the audio unit ourselves and hand that same instance to
// the host when it does ask (the generated factory method is idempotent).
// JUCE's AUv3 wrapper solves it the same way, from -loadView.
- (void)_bootstrapAudioUnit
{
  if (self.audioUnit || _bootstrapAttempted) return;
  _bootstrapAttempted = YES;
  AUV3LOG("bootstrap: creating the audio unit ourselves (no host factory call yet)");

  AudioComponentDescription desc = {};
  if (![self _ownComponentDescription:&desc])
  {
    AUV3ERR("bootstrap: no AudioComponents entry for %{public}s",
            NSStringFromClass([self class]).UTF8String);
    return;
  }

  NSError *error = nil;
  if (![self createAudioUnitWithComponentDescription:desc error:&error])
  {
    AUV3ERR("bootstrap: audio unit creation failed: %{public}s",
            error ? error.localizedDescription.UTF8String : "unknown error");
  }
}

// AUv3's only channel for a plugin-declared window size is this view
// controller's preferredContentSize (plus the view's frame), and hosts read it
// before the view is ever put into a window. CLAP can answer that early: once
// plugin->init() has run, gui->create() is the only precondition for
// get_size() — set_parent() and show() come afterwards (see clap/ext/gui.h).
// So create the GUI here and keep it until the view goes away.
- (BOOL)_ensureGUIPrepared
{
  if (_guiPrepared) return YES;
  if (_preparing) return NO;
  if (!self.audioUnit) return NO;  // factory hasn't run yet — setAudioUnit: retries

  if (!NSThread.isMainThread)
  {
    // gui->create() builds a native view and preferredContentSize is an AppKit
    // property — both main thread only. Out-of-process AUv3 calls into us from
    // XPC worker threads, so hand the work over and let this read fall back to
    // the placeholder; the KVO notification carries the real size shortly after.
    AUV3LOG("_ensureGUIPrepared: off main thread, deferring");
    dispatch_async(dispatch_get_main_queue(), ^{
      [self _ensureGUIPrepared];
    });
    return NO;
  }

  _preparing = YES;

  // gui_request_resize can fire from inside gui->create(), so establish the
  // back-reference first.
  [self.audioUnit setViewController:self];

  uint32_t w = 0, h = 0;
  const BOOL created = [self.audioUnit prepareGUIAndReturnWidth:&w height:&h];
  _preparing = NO;

  if (!created) return NO;  // no gui extension, or the plugin refused the api

  _guiPrepared = YES;
  AUV3LOG("GUI created, size=%ux%u", w, h);
  if (w > 0 && h > 0) [self _publishSizeWidth:w height:h];
  return YES;
}

// Tell the host how big the plugin wants to be. Also called by the audio unit
// when the plugin itself requests a resize.
- (void)_publishSizeWidth:(uint32_t)width height:(uint32_t)height
{
  // CGRectMake / CGSizeMake work identically on macOS and iOS;
  // NSMakeRect / NSMakeSize are AppKit-only.
  _pluginSize = CGSizeMake(width, height);
  _sizeKnown = YES;

  // Before the view exists there is nothing to resize and nobody observing —
  // loadView applies the size when it builds the view.
  if (!self.isViewLoaded) return;

  self.view.frame = CGRectMake(0, 0, width, height);

  // Explicit KVO notifications — required for the remote proxy to
  // forward preferredContentSize changes across the XPC boundary
  // to the host process.
  [self willChangeValueForKey:@"preferredContentSize"];
  self.preferredContentSize = _pluginSize;
  [self didChangeValueForKey:@"preferredContentSize"];
}

- (CGSize)preferredContentSize
{
  // Some hosts read the size before they ever touch .view — our own AUv3
  // standalone does (AUv3HostAppDelegate.mm). Make the first read the real one.
  if (!_sizeKnown)
  {
    [self _bootstrapAudioUnit];
    [self _ensureGUIPrepared];
  }
  // Answer from the plugin, not from AppKit's stored property: -setView:
  // overwrites that with the view's frame.
  return _sizeKnown ? _pluginSize : [super preferredContentSize];
}

// Convergence point for embedding the GUI. Called from multiple triggers:
// - viewDidMoveToWindow / viewDidMoveToSuperview (in-process)
// - viewDidAppear (out-of-process)
// Parents the GUI once all preconditions are met. Guarded by _guiParented.
- (void)_tryCreateGUI
{
  if (_guiParented) return;
  if (!self.audioUnit) return;
  if (!self.isViewLoaded || !self.view.window) return;

  // Normally a no-op — the GUI already exists from the size query.
  if (![self _ensureGUIPrepared]) return;

  if ([self.audioUnit parentGUIInView:self.view]) _guiParented = YES;
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
    [self _destroyGUI];
  }
}

// Drop the CLAP GUI. The last published size stays in preferredContentSize —
// the host asked for the size the plugin wants, and that hasn't changed.
- (void)_destroyGUI
{
  if (!_guiPrepared) return;
  _guiPrepared = NO;
  _guiParented = NO;
  [self.audioUnit destroyGUI];
}

- (void)viewDidLoad
{
  [super viewDidLoad];
  // loadView already created the GUI and published its size. If the view is
  // already in a window (LoadInProcess), parent it right away; otherwise
  // viewDidAppear / (view)didMoveToWindow does it later.
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
  if (!_guiParented) return;

  CGRect bounds = self.view.bounds;
  if (bounds.size.width > 0 && bounds.size.height > 0)
  {
    // Propagate host-initiated container resize to the CLAP plugin
    if ([self.audioUnit canResizeGUI])
    {
      [self.audioUnit setGUISize:(uint32_t)bounds.size.width height:(uint32_t)bounds.size.height];
    }

    // adjust_size may have snapped the container bounds (fixed aspect ratio,
    // step sizes), and the plugin laid out at the snapped size — so the subview
    // has to get that size too, not the raw bounds.
    CGRect childFrame = bounds;
    uint32_t agreedWidth = 0, agreedHeight = 0;
    if ([self.audioUnit prepareGUIAndReturnWidth:&agreedWidth height:&agreedHeight] && agreedWidth > 0 &&
        agreedHeight > 0)
    {
      childFrame = CGRectMake(0, 0, agreedWidth, agreedHeight);
    }

    for (CLAPWRAP_ViewClass *subview in self.view.subviews)
    {
      subview.frame = childFrame;
    }
  }
}

#if TARGET_OS_OSX
- (void)viewDidDisappear
{
  [self _destroyGUI];
  [super viewDidDisappear];
}
#else
- (void)viewDidDisappear:(BOOL)animated
{
  [self _destroyGUI];
  [super viewDidDisappear:animated];
}
#endif

- (void)dealloc
{
  [self _destroyGUI];
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
