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

#include <iostream>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include <map>

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
    if (_plugin)
    {
      _os_attached.off();
      _plugin->terminate();
      _plugin.reset();
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
  self = [super initWithComponentDescription:componentDescription options:options error:outError];
  if (!self) return nil;

  _impl = std::make_unique<free_audio::auv3_wrapper::AUv3ImplDetail>();
  _impl->_audioUnit = self;
  _impl->_clapname = [clapName UTF8String];
  _impl->_clapid = clapId ? [clapId UTF8String] : "";
  _impl->_idx = clapIndex;

  // Load CLAP library
  if (!_library.hasEntryPoint())
  {
    if (_impl->_clapname.empty())
    {
      std::cout << "[ERROR] auv3: _clapname empty and no internal entry point" << std::endl;
      if (outError)
        *outError = [NSError errorWithDomain:@"ClapAUv3" code:-1
                                    userInfo:@{NSLocalizedDescriptionKey : @"CLAP name is empty"}];
      return nil;
    }

    auto csp = Clap::getValidCLAPSearchPaths();
    auto it = std::find_if(csp.begin(), csp.end(),
                           [&](const auto &cs)
                           {
                             auto fp = cs / (_impl->_clapname + ".clap");
                             return fs::is_directory(fp) && _library.load(fp);
                           });

    if (it != csp.end())
    {
      std::cout << "[clap-wrapper] auv3 loaded clap from " << it->u8string() << std::endl;
    }
    else
    {
      std::cout << "[ERROR] auv3: cannot load clap" << std::endl;
      if (outError)
        *outError = [NSError errorWithDomain:@"ClapAUv3" code:-2
                                    userInfo:@{NSLocalizedDescriptionKey : @"Cannot load CLAP plugin"}];
      return nil;
    }
  }

  // Find the plugin descriptor
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
    std::cout << "[ERROR] auv3: cannot determine plugin description" << std::endl;
    if (outError)
      *outError = [NSError errorWithDomain:@"ClapAUv3" code:-3
                                  userInfo:@{NSLocalizedDescriptionKey : @"Cannot find CLAP plugin descriptor"}];
    return nil;
  }

  std::cout << "[clap-wrapper] auv3: Initialized '" << _impl->_desc->id << "' / '"
            << _impl->_desc->name << "' / '" << _impl->_desc->version << "'" << std::endl;

  // Create the plugin instance
  _impl->_plugin = Clap::Plugin::createInstance(_library._pluginFactory, _impl->_desc->id, _impl.get());
  if (!_impl->_plugin)
  {
    std::cout << "[ERROR] auv3: the clap did not create an instance" << std::endl;
    if (outError)
      *outError = [NSError errorWithDomain:@"ClapAUv3" code:-4
                                  userInfo:@{NSLocalizedDescriptionKey : @"CLAP plugin instance creation failed"}];
    return nil;
  }

  _impl->_plugin->initialize();
  _impl->_os_attached.on();

  // Build audio bus arrays from the CLAP audio port info
  [self _buildBusArrays];

  _renderResourcesAllocated = NO;

  return self;
}

- (void)dealloc
{
  if (_impl && _impl->_plugin)
  {
    _impl->_os_attached.off();
    _impl->_plugin->terminate();
    _impl->_plugin.reset();
  }
  _impl.reset();
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
  // Accept format changes
  return YES;
}

// --- State save/restore ---

- (NSDictionary<NSString *, id> *)fullState
{
  NSMutableDictionary *state = [[super fullState] mutableCopy];
  if (!state) state = [NSMutableDictionary new];

  if (_impl && _impl->_plugin && _impl->_plugin->_ext._state)
  {
    Clap::StateMemento chunk;
    if (_impl->_plugin->_ext._state->save(_impl->_plugin->_plugin, chunk))
    {
      NSData *clapState = [NSData dataWithBytes:chunk.data() length:chunk.size()];
      state[@"clapState"] = clapState;
    }
  }

  return state;
}

- (void)setFullState:(NSDictionary<NSString *, id> *)fullState
{
  [super setFullState:fullState];

  if (_impl && _impl->_plugin && _impl->_plugin->_ext._state)
  {
    NSData *clapState = fullState[@"clapState"];
    if (clapState)
    {
      Clap::StateMemento chunk;
      chunk.setData((const uint8_t *)[clapState bytes], [clapState length]);
      _impl->_plugin->_ext._state->load(_impl->_plugin->_plugin, chunk);
    }
  }
}

// --- Render resources ---

- (BOOL)allocateRenderResourcesAndReturnError:(NSError **)outError
{
  if (![super allocateRenderResourcesAndReturnError:outError])
  {
    return NO;
  }

  if (!_impl || !_impl->_plugin)
  {
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

  auto guarantee_mainthread = _impl->_plugin->AlwaysMainThread();

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

  // Create and set up the process adapter
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
  _impl->_plugin->activate();
  _impl->_plugin->start_processing();
  _impl->_initialized = true;

  // Wire up the parameter value observer
  if (_impl->_parameterTree)
  {
    __weak typeof(self) weakSelf = self;
    _impl->_parameterTree.implementorValueObserver = ^(AUParameter *param, AUValue value) {
      __strong typeof(weakSelf) strongSelf = weakSelf;
      if (strongSelf && strongSelf->_impl && strongSelf->_impl->_processAdapter)
      {
        strongSelf->_impl->_processAdapter->addParameterEvent((clap_id)param.address, (double)value, 0);
      }
    };
  }

  _renderResourcesAllocated = YES;
  return YES;
}

- (void)deallocateRenderResources
{
  if (_impl && _impl->_plugin && _impl->_initialized)
  {
    auto guarantee_mainthread = _impl->_plugin->AlwaysMainThread();
    _impl->_plugin->stop_processing();
    _impl->_plugin->deactivate();
    _impl->_initialized = false;
  }

  _impl->_processAdapter.reset();
  _renderResourcesAllocated = NO;

  [super deallocateRenderResources];
}

// --- Render block ---

- (AUInternalRenderBlock)internalRenderBlock
{
  // Capture a raw pointer to the C++ impl for use in the render block.
  // This is safe because the render block's lifetime is bounded by
  // allocateRenderResources / deallocateRenderResources.
  auto *adapter = _impl->_processAdapter.get();

  return ^AUAudioUnitStatus(AudioUnitRenderActionFlags *actionFlags,
                             const AudioTimeStamp *timestamp,
                             AUAudioFrameCount frameCount,
                             NSInteger outputBusNumber,
                             AudioBufferList *outputData,
                             const AURenderEvent *realtimeEventListHead,
                             AURenderPullInputBlock __unsafe_unretained pullInputBlock) {
    if (!adapter) return kAudioUnitErr_Uninitialized;

    return adapter->process(actionFlags, timestamp, frameCount, outputBusNumber, outputData,
                            realtimeEventListHead, pullInputBlock);
  };
}

// --- GUI methods for the view controller ---

- (BOOL)createGUIInView:(NSView *)parentView width:(uint32_t *)outWidth height:(uint32_t *)outHeight
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return NO;

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
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui) return;

  _impl->_plugin->_ext._gui->hide(_impl->_plugin->_plugin);
  _impl->_plugin->_ext._gui->destroy(_impl->_plugin->_plugin);
  _impl->_guiParentView = nil;
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

- (void)requestViewControllerWithCompletionHandler:(void (^)(AUViewControllerBase *_Nullable))completionHandler
{
  if (!_impl || !_impl->_plugin || !_impl->_plugin->_ext._gui)
  {
    completionHandler(nil);
    return;
  }

  // Check if the CLAP plugin supports Cocoa GUI
  if (!_impl->_plugin->_ext._gui->is_api_supported(_impl->_plugin->_plugin, CLAP_WINDOW_API_COCOA, false))
  {
    completionHandler(nil);
    return;
  }

  // Create the view controller on the main thread
  dispatch_async(dispatch_get_main_queue(), ^{
    ClapAUv3ViewController *vc = [[ClapAUv3ViewController alloc] init];
    vc.audioUnit = self;
    completionHandler(vc);
  });
}

@end

// -----------------------------------------------------------------------
// ClapAUv3ViewController implementation (also serves as AUAudioUnitFactory)
// -----------------------------------------------------------------------

@implementation ClapAUv3ViewController

- (void)loadView
{
  // Create a plain NSView as the container
  self.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 480, 360)];
}

- (void)viewDidLoad
{
  [super viewDidLoad];

  if (!self.audioUnit) return;

  uint32_t w = 0, h = 0;
  if ([self.audioUnit createGUIInView:self.view width:&w height:&h])
  {
    if (w > 0 && h > 0)
    {
      self.preferredContentSize = NSMakeSize(w, h);
      self.view.frame = NSMakeRect(0, 0, w, h);
    }
  }
}

- (void)viewDidDisappear
{
  [self.audioUnit destroyGUI];
  [super viewDidDisappear];
}

// --- AUAudioUnitFactory ---
// Base implementation -- subclasses generated by build-helper override this.

- (AUAudioUnit *)createAudioUnitWithComponentDescription:(AudioComponentDescription)desc
                                                   error:(NSError **)error
{
  if (error)
    *error = [NSError errorWithDomain:@"ClapAUv3" code:-100
                             userInfo:@{NSLocalizedDescriptionKey : @"Base factory should not be called directly"}];
  return nil;
}

- (void)beginRequestWithExtensionContext:(NSExtensionContext *)context
{
  // Required by NSExtensionRequestHandling protocol.
}

@end

#pragma clang diagnostic pop
