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
#include <iostream>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <map>

static os_log_t _auv3Log() {
  static os_log_t log = os_log_create("org.clap-wrapper.auv3", "wrapper");
  return log;
}
#define AUV3LOG(...) os_log(_auv3Log(), __VA_ARGS__)
#define AUV3ERR(...) os_log_error(_auv3Log(), __VA_ARGS__)

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

class AUv3ImplDetail : public Clap::IHost,
                       public Clap::IAutomation,
                       public os::IPlugObject
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
      AUV3LOG("~AUv3ImplDetail: calling _os_attached.off()");
      _os_attached.off();
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
  bool _midi_wants_midi_input = false;
  std::vector<NSString *> _midiOutputNames;

  // Parameters
  AUParameterTree *_parameterTree = nil;

  // Hosting
  std::string _clapname;
  std::string _clapid;
  int _idx = 0;
  os::State _os_attached;
  std::string _hostname = "CLAP-as-AUv3";
  std::atomic<bool> _initialized{false};
  std::atomic_bool _requestUICallback{false};

  // Back-reference to the ObjC audio unit (weak to avoid retain cycle)
  __weak ClapAUv3AudioUnit *_audioUnit = nil;

  // The NSView that the CLAP GUI is parented to (set by createGUIInView:)
  __weak NSView *_guiParentView = nil;

  // Queue for audio -> UI thread parameter notifications
  ClapWrapper::detail::shared::fixedqueue<queueEvent, 8192> _queueToUI;

  // --- IHost ---
  void mark_dirty() override {}
  void restartPlugin() override {}

  void request_callback() override { _requestUICallback = true; }

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

  void setupMIDIBusses(const clap_plugin_t *plugin,
                       const clap_plugin_note_ports_t *noteports) override
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

  void setupParameters(const clap_plugin_t *plugin,
                       const clap_plugin_params_t *params) override
  {
    _parameterTree = Clap::AUv3::createParameterTree(plugin, params);
  }

  void param_rescan(clap_param_rescan_flags flags) override
  {
    // TODO: Rebuild parameter tree when plugin requests rescan
    std::cout << "[clap-wrapper] auv3: param_rescan requested (not yet fully implemented)" << std::endl;
  }

  void param_clear(clap_id param, clap_param_clear_flags flags) override {}
  void param_request_flush() override {}

  void latency_changed() override
  {
    // AUv3 handles latency via the latency property - hosts observe it via KVO
  }

  void tail_changed() override
  {
    // AUv3 handles tail time via the tailTime property
  }

  bool gui_can_resize() override
  {
    if (_plugin && _plugin->_ext._gui)
      return _plugin->_ext._gui->can_resize(_plugin->_plugin);
    return false;
  }

  bool gui_request_resize(uint32_t width, uint32_t height) override
  {
    // Notify the host that the plugin wants to resize
    if (_guiParentView)
    {
      dispatch_async(dispatch_get_main_queue(), ^{
        NSView *view = _guiParentView;
        if (view)
        {
          NSWindow *window = view.window;
          if (window)
          {
            [window setContentSize:NSMakeSize(width, height)];
          }
        }
      });
      return true;
    }
    return false;
  }

  bool gui_request_show() override { return false; }
  bool gui_request_hide() override { return false; }

  bool register_timer(uint32_t period_ms, clap_id *timer_id) override { return false; }
  bool unregister_timer(clap_id timer_id) override { return false; }

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

  bool track_info_get(clap_track_info_t *info) override { return false; }

  bool supportsContextMenu() const override { return false; }
  bool context_menu_populate(const clap_context_menu_target_t *target,
                             const clap_context_menu_builder_t *builder) override
  {
    return false;
  }
  bool context_menu_perform(const clap_context_menu_target_t *target, clap_id action_id) override
  {
    return false;
  }
  bool context_menu_can_popup() override { return false; }
  bool context_menu_popup(const clap_context_menu_target_t *target, int32_t screen_index,
                          int32_t x, int32_t y) override
  {
    return false;
  }

  // --- IAutomation ---
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

  // --- IPlugObject ---
  void onIdle() override
  {
    if (!_plugin) return;

    if (_requestUICallback.exchange(false))
    {
      auto guard = _plugin->AlwaysMainThread();
      _plugin->_plugin->on_main_thread(_plugin->_plugin);
    }

    // Process queued parameter changes from audio thread
    queueEvent evt;
    while (_queueToUI.pop(evt))
    {
      switch (evt._type)
      {
        case queueEvent::type::editvalue:
        {
          if (_parameterTree)
          {
            AUParameter *param = [_parameterTree parameterWithAddress:(AUParameterAddress)evt._data._value.param_id];
            if (param)
            {
              param.value = (AUValue)evt._data._value.value;
            }
          }
          break;
        }
        default:
          break;
      }
    }
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
  AUV3LOG("initWithComponentDescription: thread=%{public}s", [NSThread.currentThread.name UTF8String] ?: "unnamed");

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

    AUV3LOG("init: name='%{public}s' id='%{public}s' idx=%d",
            _impl->_clapname.c_str(), _impl->_clapid.c_str(), _impl->_idx);

    // Load CLAP library
    if (!_library.hasEntryPoint())
    {
      AUV3LOG("init: library has no entry point, searching for CLAP");
      if (_impl->_clapname.empty())
      {
        AUV3ERR("init: _clapname empty and no internal entry point");
        if (outError)
          *outError = [NSError errorWithDomain:@"ClapAUv3" code:-1
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
          *outError = [NSError errorWithDomain:@"ClapAUv3" code:-2
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
        *outError = [NSError errorWithDomain:@"ClapAUv3" code:-3
                                    userInfo:@{NSLocalizedDescriptionKey : @"Cannot find CLAP plugin descriptor"}];
      return nil;
    }

    AUV3LOG("init: found descriptor id='%{public}s' name='%{public}s' version='%{public}s'",
            _impl->_desc->id, _impl->_desc->name, _impl->_desc->version);

    // Create the plugin instance
    AUV3LOG("init: creating plugin instance via factory");
    _impl->_plugin = Clap::Plugin::createInstance(_library._pluginFactory, _impl->_desc->id, _impl.get());
    if (!_impl->_plugin)
    {
      AUV3ERR("init: factory returned null plugin instance");
      if (outError)
        *outError = [NSError errorWithDomain:@"ClapAUv3" code:-4
                                    userInfo:@{NSLocalizedDescriptionKey : @"CLAP plugin instance creation failed"}];
      return nil;
    }
    AUV3LOG("init: plugin instance created successfully");

    AUV3LOG("init: calling plugin->initialize()");
    _impl->_plugin->initialize();
    AUV3LOG("init: calling _os_attached.on()");
    _impl->_os_attached.on();

    // Build audio bus arrays from the CLAP audio port info
    AUV3LOG("init: building bus arrays (inputs=%zu outputs=%zu)",
            _impl->_inputBusInfos.size(), _impl->_outputBusInfos.size());
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
      *outError = [NSError errorWithDomain:@"ClapAUv3" code:e
                                  userInfo:@{NSLocalizedDescriptionKey : @"C++ int exception during init"}];
    return nil;
  }
  catch (const std::exception &e)
  {
    AUV3ERR("init: caught std::exception: %{public}s", e.what());
    if (outError)
      *outError = [NSError errorWithDomain:@"ClapAUv3" code:-99
                                  userInfo:@{NSLocalizedDescriptionKey : [NSString stringWithUTF8String:e.what()]}];
    return nil;
  }
  catch (...)
  {
    AUV3ERR("init: caught unknown C++ exception");
    if (outError)
      *outError = [NSError errorWithDomain:@"ClapAUv3" code:-98
                                  userInfo:@{NSLocalizedDescriptionKey : @"Unknown C++ exception during init"}];
    return nil;
  }

  return self;
}

- (void)dealloc
{
  AUV3LOG("dealloc: entered (self=%p, thread=%{public}s)", self,
          [NSThread.currentThread.name UTF8String] ?: "unnamed");
  AUV3LOG("dealloc: _impl=%{public}s, _plugin=%{public}s",
          _impl ? "valid" : "null",
          (_impl && _impl->_plugin) ? "valid" : "null");

  if (_impl && _impl->_plugin)
  {
    AUV3LOG("dealloc: calling _os_attached.off()");
    _impl->_os_attached.off();
    AUV3LOG("dealloc: calling _plugin->terminate()");
    _impl->_plugin->terminate();
    AUV3LOG("dealloc: calling _plugin.reset()");
    _impl->_plugin.reset();
    AUV3LOG("dealloc: plugin teardown complete");
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
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:self.outputBusses.count > 0 ? 44100.0 : 44100.0
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
  _inputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self busType:AUAudioUnitBusTypeInput busses:inputs];

  // Build output bus array
  NSMutableArray<AUAudioUnitBus *> *outputs = [NSMutableArray new];
  for (auto &busInfo : _impl->_outputBusInfos)
  {
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:44100.0
                                                                          channels:busInfo.channelCount];
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
  _outputBusArray = [[AUAudioUnitBusArray alloc] initWithAudioUnit:self busType:AUAudioUnitBusTypeOutput busses:outputs];
}

- (void)_wireParameterObserver
{
  __weak typeof(self) weakSelf = self;

  _impl->_parameterTree.implementorValueObserver = ^(AUParameter *param, AUValue value) {
    __strong typeof(weakSelf) strongSelf = weakSelf;
    if (!strongSelf || !strongSelf->_impl) return;
    if (!strongSelf->_impl->_plugin || !strongSelf->_impl->_plugin->_ext._params) return;

    // When render resources are allocated, parameter changes arrive via the
    // render event list (AURenderEventParameter) — the thread-safe path.
    // Do NOT call addParameterEvent here as it races with process() on
    // the render thread (both touch _events/_eventindices without locking).
    if (strongSelf->_renderResourcesAllocated) return;

    // Non-realtime path: push directly to the CLAP plugin via flush.
    // This is safe because flush must only be called when not processing.
    auto *plugin = strongSelf->_impl->_plugin->_plugin;
    auto *ext_params = strongSelf->_impl->_plugin->_ext._params;

    clap_event_param_value_t ev = {};
    ev.header.size = sizeof(ev);
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.time = 0;
    ev.header.flags = 0;
    ev.param_id = (clap_id)param.address;
    ev.value = (double)value;
    ev.port_index = -1;
    ev.key = -1;
    ev.channel = -1;
    ev.note_id = -1;

    // Build a single-event input list
    const clap_event_header_t *evPtr = &ev.header;
    clap_input_events_t in_events = {};
    in_events.ctx = &evPtr;
    in_events.size = [](const clap_input_events_t *) -> uint32_t { return 1; };
    in_events.get = [](const clap_input_events_t *list, uint32_t) -> const clap_event_header_t * {
      return *static_cast<const clap_event_header_t *const *>(list->ctx);
    };

    clap_output_events_t out_events = {};
    out_events.ctx = nullptr;
    out_events.try_push = [](const clap_output_events_t *, const clap_event_header_t *) -> bool {
      return true;
    };

    ext_params->flush(plugin, &in_events, &out_events);
  };
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

- (NSTimeInterval)latency
{
  if (_impl && _impl->_plugin && _impl->_plugin->_ext._latency)
  {
    uint32_t samples = _impl->_plugin->_ext._latency->get(_impl->_plugin->_plugin);
    return (double)samples / self.outputBusses[0].format.sampleRate;
  }
  return 0;
}

- (NSTimeInterval)tailTime
{
  if (_impl && _impl->_plugin && _impl->_plugin->_ext._tail)
  {
    uint32_t samples = _impl->_plugin->_ext._tail->get(_impl->_plugin->_plugin);
    if (samples == UINT32_MAX) return INFINITY;
    return (double)samples / self.outputBusses[0].format.sampleRate;
  }
  return 0;
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
      _impl->_plugin->_ext._state->load(_impl->_plugin->_plugin, chunk);
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
      *outError = [NSError errorWithDomain:@"ClapAUv3" code:-10
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
  AUV3LOG("allocateRenderResources: sampleRate=%.0f maxFrames=%u",
          sampleRate, (unsigned)self.maximumFramesToRender);

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
  AUV3LOG("allocateRenderResources: input busses=%zu output busses=%zu",
          inputChs.size(), outputChs.size());

  // Create and set up the process adapter
  AUV3LOG("allocateRenderResources: creating process adapter");
  _impl->_processAdapter = std::make_unique<Clap::AUv3::ProcessAdapter>();
  _impl->_processAdapter->setupProcessing(
      (uint32_t)inputChs.size(), inputChs.empty() ? nullptr : inputChs.data(),
      (uint32_t)outputChs.size(), outputChs.empty() ? nullptr : outputChs.data(),
      _impl->_plugin->_plugin, _impl->_plugin->_ext._params, _impl.get(),
      self.maximumFramesToRender, _impl->_midi_preferred_dialect);

  // Set transport state block
  _impl->_processAdapter->setTransportStateBlock(self.transportStateBlock);

  // Set MIDI output block
  _impl->_processAdapter->midiOutputEventBlock = self.MIDIOutputEventBlock;

  // Activate the CLAP plugin
  AUV3LOG("allocateRenderResources: calling activate()");
  _impl->_plugin->activate();
  AUV3LOG("allocateRenderResources: calling start_processing()");
  _impl->_plugin->start_processing();
  _impl->_initialized = true;

  _renderResourcesAllocated = YES;
  AUV3LOG("allocateRenderResources: completed successfully");
  return YES;
}

- (void)deallocateRenderResources
{
  AUV3LOG("deallocateRenderResources: entered (thread=%{public}s)",
          [NSThread.currentThread.name UTF8String] ?: "unnamed");

  if (_impl && _impl->_plugin && _impl->_initialized)
  {
    auto guarantee_mainthread = _impl->_plugin->AlwaysMainThread();
    AUV3LOG("deallocateRenderResources: calling stop_processing()");
    _impl->_plugin->stop_processing();
    AUV3LOG("deallocateRenderResources: calling deactivate()");
    _impl->_plugin->deactivate();
    _impl->_initialized = false;
  }

  AUV3LOG("deallocateRenderResources: resetting process adapter");
  _impl->_processAdapter.reset();
  _renderResourcesAllocated = NO;

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

  return ^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags,
                             const AudioTimeStamp *timestamp,
                             AUAudioFrameCount frameCount,
                             NSInteger outputBusNumber,
                             AudioBufferList *outputData,
                             const AURenderEvent *realtimeEventListHead,
                             AURenderPullInputBlock __unsafe_unretained pullInputBlock) {
    if (!impl || !impl->_processAdapter) return kAudioUnitErr_Uninitialized;

    return impl->_processAdapter->process(actionFlags, timestamp, frameCount, outputBusNumber,
                                          outputData, realtimeEventListHead, pullInputBlock);
  };
}

// --- GUI methods for the view controller ---

- (BOOL)createGUIInView:(NSView *)parentView width:(uint32_t *)outWidth height:(uint32_t *)outHeight
{
  AUV3LOG("createGUIInView: entered (parentView=%p)", parentView);
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui)
  {
    AUV3LOG("createGUIInView: no GUI extension available");
    return NO;
  }

  auto *gui = _impl->_plugin->_ext._gui;
  auto *plugin = _impl->_plugin->_plugin;

  if (!gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false)) return NO;

  if (!gui->create(plugin, CLAP_WINDOW_API_COCOA, false)) return NO;

  gui->set_scale(plugin, 1.0);

  uint32_t w = 0, h = 0;
  gui->get_size(plugin, &w, &h);

  if (gui->can_resize(plugin))
  {
    gui->adjust_size(plugin, &w, &h);
  }

  clap_window_t window;
  window.api = CLAP_WINDOW_API_COCOA;
  window.cocoa = (__bridge void *)parentView;
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
  AUV3LOG("destroyGUI: entered");
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui)
  {
    AUV3LOG("destroyGUI: no GUI extension, nothing to destroy");
    return;
  }

  AUV3LOG("destroyGUI: hiding and destroying GUI");
  _impl->_plugin->_ext._gui->hide(_impl->_plugin->_plugin);
  _impl->_plugin->_ext._gui->destroy(_impl->_plugin->_plugin);
  _impl->_guiParentView = nil;
  AUV3LOG("destroyGUI: completed");
}

- (BOOL)canResizeGUI
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;
  return _impl->_plugin->_ext._gui->can_resize(_impl->_plugin->_plugin) ? YES : NO;
}

- (BOOL)setGUISize:(uint32_t)width height:(uint32_t)height
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;
  return _impl->_plugin->_ext._gui->set_size(_impl->_plugin->_plugin, width, height) ? YES : NO;
}

// --- View controller ---
// requestViewControllerWithCompletionHandler: is NOT overridden.
// The default AUAudioUnit implementation returns the NSExtensionPrincipalClass
// view controller (the factory VC that created this AU). This is the same
// pattern used by the VST3 SDK's AUv3 wrapper.

@end

// -----------------------------------------------------------------------
// ClapAUv3ViewController implementation (also serves as AUAudioUnitFactory)
// -----------------------------------------------------------------------

@implementation ClapAUv3ViewController

- (void)loadView
{
  AUV3LOG("loadView: entered (thread=%{public}s)",
          [NSThread.currentThread.name UTF8String] ?: "unnamed");
  NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 0, 0)];
  view.autoresizingMask = NSViewNotSizable;
  view.translatesAutoresizingMaskIntoConstraints = YES;
  [self setView:view];
  AUV3LOG("loadView: completed");
}

// Custom setter: trigger GUI creation when audioUnit is set and view is already loaded.
// This matches the VST3 SDK's setAudioUnit: → makePlugView pattern.
- (void)setAudioUnit:(ClapAUv3AudioUnit *)audioUnit
{
  AUV3LOG("setAudioUnit: entered (audioUnit=%p, viewLoaded=%d, thread=%{public}s)",
          audioUnit, [self isViewLoaded],
          [NSThread.currentThread.name UTF8String] ?: "unnamed");
  _audioUnit = audioUnit;
  // Do NOT create the GUI here. The GUI is created lazily when the host
  // explicitly shows the view (viewDidAppear / viewDidLayout). Creating it
  // eagerly blocks the main thread (JUCE MessageManager init), which prevents
  // the appex from processing subsequent XPC messages — causing auval WARM
  // timeout (-10863) and similar hangs in headless hosts.
}

- (void)_createPluginGUI
{
  AUV3LOG("_createPluginGUI: entered (audioUnit=%p)", self.audioUnit);
  if (!self.audioUnit)
  {
    AUV3LOG("_createPluginGUI: no audioUnit set, skipping");
    return;
  }

  uint32_t w = 0, h = 0;
  if ([self.audioUnit createGUIInView:self.view width:&w height:&h])
  {
    AUV3LOG("_createPluginGUI: GUI created, size=%ux%u", w, h);
    if (w > 0 && h > 0)
    {
      self.preferredContentSize = NSMakeSize(w, h);
      self.view.frame = NSMakeRect(0, 0, w, h);
    }
  }
  else
  {
    AUV3LOG("_createPluginGUI: createGUIInView returned NO");
  }
}

- (void)viewDidLoad
{
  AUV3LOG("viewDidLoad: entered");
  [super viewDidLoad];
  // Do NOT create the GUI here — defer to viewDidAppear so the CLAP GUI
  // is only created when the host actually displays the view.
  AUV3LOG("viewDidLoad: completed");
}

- (void)viewDidAppear
{
  AUV3LOG("viewDidAppear: entered (audioUnit=%p)", self.audioUnit);
  [super viewDidAppear];
  [self _createPluginGUI];
  AUV3LOG("viewDidAppear: completed");
}

- (void)viewDidDisappear
{
  AUV3LOG("viewDidDisappear: entered");
  [self.audioUnit destroyGUI];
  [super viewDidDisappear];
  AUV3LOG("viewDidDisappear: completed");
}

// --- AUAudioUnitFactory ---
// Base implementation -- subclasses generated by build-helper override this.

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
  AUV3ERR("createAudioUnitWithComponentDescription: BASE class called — subclass should override");
  if (error)
    *error = [NSError errorWithDomain:@"ClapAUv3" code:-100
                             userInfo:@{NSLocalizedDescriptionKey : @"Base factory should not be called directly"}];
  return nil;
}

- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context
{
  AUV3LOG("beginRequestWithExtensionContext: entered (context=%p)", context);
}

@end

#pragma clang diagnostic pop
