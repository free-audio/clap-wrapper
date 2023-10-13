#include "clap_proxy.h"
#include "detail/clap/fsutil.h"
#include <clap/helpers/host.hxx>
#include <cstring>
#include <clap/helpers/plugin-proxy.hxx>

#if MAC || LIN
#include <iostream>
#define OutputDebugString(x) std::cout << __FILE__ << ":" << __LINE__ << " " << (x) << std::endl;
#define OutputDebugStringA(x) std::cout << __FILE__ << ":" << __LINE__ << " " << (x) << std::endl;
#endif
#if WIN
#include <crtdbg.h>
#endif

template class clap::helpers::Host<Clap::Plugin_MH, Clap::Plugin_CL>;
template class clap::helpers::PluginProxy<Clap::Plugin_MH, Clap::Plugin_CL>;

namespace Clap
{
std::shared_ptr<Plugin> Plugin::createInstance(const clap_plugin_factory* fac, const std::string& id,
                                               Clap::IHost* host)
{
  auto plug = std::shared_ptr<Plugin>(new Plugin(host));
  auto instance = fac->create_plugin(fac, plug->clapHost(), id.c_str());
  plug->connectClap(instance);

  return plug;
}

std::shared_ptr<Plugin> Plugin::createInstance(const clap_plugin_factory* fac, size_t idx,
                                               Clap::IHost* host)
{
  auto pc = fac->get_plugin_count(fac);
  if (idx >= pc) return nullptr;
  auto desc = fac->get_plugin_descriptor(fac, (uint32_t)idx);
  return createInstance(fac, desc->id, host);
}

std::shared_ptr<Plugin> Plugin::createInstance(Clap::Library& library, size_t index, IHost* host)
{
  if (library.plugins.size() > index)
  {
    auto plug = std::shared_ptr<Plugin>(new Plugin(host));
    auto instance = library._pluginFactory->create_plugin(library._pluginFactory, plug->clapHost(),
                                                          library.plugins[index]->id);
    plug->connectClap(instance);

    return plug;
  }
  return nullptr;
}

Plugin::Plugin(IHost* host)
  : PluginHostBase{"Clap-As-VST3-Wrapper", "defiant nerd", "https://www.defiantnerd.com", "0.0.1"}
  , _parentHost(host)
{
}

template <typename T>
void getExtension(const clap_plugin_t* plugin, T& ref, const char* name)
{
  ref = static_cast<T>(plugin->get_extension(plugin, name));
}

void Plugin::connectClap(const clap_plugin_t* clap)
{
  if (!clap) return;

  _pluginProxy = std::make_unique<PluginProxy>(*clap, *this);

  // initialize the plugin
  if (!_pluginProxy->init())
  {
    terminate();
    return;
  }

  if (_pluginProxy->canUseGui())
  {
    const char* api;
#if WIN
    api = CLAP_WINDOW_API_WIN32;
#endif
#if MAC
    api = CLAP_WINDOW_API_COCOA;
#endif
#if LIN
    api = CLAP_WINDOW_API_X11;
#endif

    if (!_pluginProxy->guiIsApiSupported(api, false))
    {
      // disable GUI if not win32
      // TODO _pluginProxy->_pluginGui = nullptr;
    }
  }
}

Plugin::~Plugin()
{
  if (_pluginProxy.get())
  {
    terminate();
  }
}

void Plugin::schnick()
{
}

bool Plugin::initialize()
{
  const auto& proxy = *getProxy();
  if (proxy.canUseAudioPorts())
  {
    _parentHost->setupAudioBusses(proxy);
  }
  if (proxy.canUseNotePorts())
  {
    _parentHost->setupMIDIBusses(proxy);
  }
  if (proxy.canUseParams())
  {
    _parentHost->setupParameters(proxy);
  }

  _parentHost->setupWrapperSpecifics(proxy);
  return true;
}

void Plugin::terminate()
{
  _pluginProxy->destroy();
  _pluginProxy.reset();
}

void Plugin::setSampleRate(double sampleRate)
{
  _audioSetup.sampleRate = sampleRate;
}

void Plugin::setBlockSizes(uint32_t minFrames, uint32_t maxFrames)
{
  _audioSetup.minFrames = minFrames;
  _audioSetup.maxFrames = maxFrames;
}

bool Plugin::load(const clap_istream_t* stream) const
{
  if (_pluginProxy->canUseState())
  {
    return _pluginProxy->stateLoad(stream);
  }
  return false;
}

bool Plugin::save(const clap_ostream_t* stream) const
{
  if (_pluginProxy->canUseState())
  {
    return _pluginProxy->stateSave(stream);
  }
  return false;
}

bool Plugin::activate() const
{
  return _pluginProxy->activate(_audioSetup.sampleRate, _audioSetup.minFrames, _audioSetup.maxFrames);
}

void Plugin::deactivate() const
{
  _pluginProxy->deactivate();
}

bool Plugin::start_processing()
{
  auto thisFn = AlwaysAudioThread();
  return _pluginProxy->startProcessing();
}

void Plugin::stop_processing()
{
  auto thisFn = AlwaysAudioThread();
  _pluginProxy->stopProcessing();
}

//void Plugin::process(const clap_process_t* data)
//{
//  auto thisFn = AlwaysAudioThread();
//  _pluginProxy->process(data);
//}

const clap_plugin_gui_t* Plugin::getUI() const
{
  if (_pluginProxy->canUseGui())
  {
    if (_pluginProxy->guiIsApiSupported(CLAP_WINDOW_API_WIN32, false))
    {
      // _pluginProxy->gui...
    }
  }
  return nullptr;
}

void Plugin::latencyChanged() noexcept
{
  _parentHost->latency_changed();
}

void Plugin::tailChanged() noexcept
{
  _parentHost->tail_changed();
}

PluginProxy* Plugin::getProxy() const
{
  return _pluginProxy.get();
}

void Plugin::logLog(clap_log_severity severity, const char* msg) const noexcept
{
  std::string n;
  switch (severity)
  {
    case CLAP_LOG_DEBUG:
      n.append("PLUGIN: DEBUG: ");
      break;
    case CLAP_LOG_INFO:
      n.append("PLUGIN: INFO: ");
      break;
    case CLAP_LOG_WARNING:
      n.append("PLUGIN: WARNING: ");
      break;
    case CLAP_LOG_ERROR:
      n.append("PLUGIN: ERROR: ");
      break;
    case CLAP_LOG_FATAL:
      n.append("PLUGIN: FATAL: ");
      break;
    case CLAP_LOG_PLUGIN_MISBEHAVING:
      n.append("PLUGIN: MISBEHAVING: ");
      break;
    case CLAP_LOG_HOST_MISBEHAVING:
      n.append("PLUGIN: HOST MISBEHAVING: ");
#if WIN
      OutputDebugStringA(msg);
      _CrtDbgBreak();
#endif
#if MAC
      fprintf(stderr, "%s\n", msg);
#endif
      break;
  }
  n.append(msg);
#if WIN
  OutputDebugStringA(n.c_str());
  OutputDebugStringA("\n");
#endif
#if MAC
  fprintf(stderr, "%s\n", n.c_str());
#endif
}

bool Plugin::threadCheckIsMainThread() const noexcept
{
  if (this->_main_thread_override > 0)
  {
    return true;
  }
  return _main_thread_id == std::this_thread::get_id();
}

bool Plugin::threadCheckIsAudioThread() const noexcept
{
  if (this->_audio_thread_override > 0)
  {
    return true;
  }
  return !(_main_thread_id == std::this_thread::get_id());
}

CLAP_NODISCARD Raise Plugin::AlwaysAudioThread()
{
  return Raise(this->_audio_thread_override);
}

CLAP_NODISCARD Raise Plugin::AlwaysMainThread()
{
  return Raise(this->_main_thread_override);
}

void Plugin::paramsRescan(clap_param_rescan_flags flags) noexcept
{
  _parentHost->param_rescan(flags);
}

void Plugin::paramsClear(clap_id param, clap_param_clear_flags flags) noexcept
{
  _parentHost->param_clear(param, flags);
}
void Plugin::paramsRequestFlush() noexcept
{
  _parentHost->param_request_flush();
}

void Plugin::requestRestart() noexcept
{
  _parentHost->restartPlugin();
}

void Plugin::requestProcess() noexcept
{
  // right now, I don't know how to communicate this to the host
  // in VST3 you can't force processing...
}

void Plugin::requestCallback() noexcept
{
  _parentHost->request_callback();
}

// Registers a periodic timer.
// The host may adjust the period if it is under a certain threshold.
// 30 Hz should be allowed.
// [main-thread]
bool Plugin::timerSupportRegisterTimer(uint32_t period_ms, clap_id* timer_id) noexcept
{
  return _parentHost->register_timer(period_ms, timer_id);
}
bool Plugin::timerSupportUnregisterTimer(clap_id timer_id) noexcept
{
  return _parentHost->unregister_timer(timer_id);
}

void StateMemento::setData(const uint8_t* data, size_t size)
{
  _mem.clear();
  if (size > 0) _mem.insert(_mem.end(), &data[0], &data[size]);
  _readoffset = 0;
}

int64_t CLAP_ABI StateMemento::_read(const struct clap_istream* stream, void* buffer, uint64_t size)
{
  StateMemento* self = static_cast<StateMemento*>(stream->ctx);
  auto realsize = size;
  uint64_t endpos = self->_readoffset + realsize;
  if (endpos >= self->_mem.size())
  {
    realsize = self->_mem.size() - self->_readoffset;
  }
  if (realsize > 0)
  {
    memcpy(buffer, &self->_mem[self->_readoffset], realsize);
    self->_readoffset += realsize;
  }
  return realsize;
}

int64_t CLAP_ABI StateMemento::_write(const struct clap_ostream* stream, const void* buffer,
                                      uint64_t size)
{
  StateMemento* self = static_cast<StateMemento*>(stream->ctx);
  auto* ptr = static_cast<const uint8_t*>(buffer);
  self->_mem.insert(self->_mem.end(), &ptr[0], &ptr[size]);
  return size;
}

#if LIN
bool Plugin::posixFdSupportRegisterFd(int fd, clap_posix_fd_flags_t flags) noexcept
{
  return _parentHost->register_fd(fd, flags);
}
bool Plugin::posixFdSupportModifyFd(int fd, clap_posix_fd_flags_t flags) noexcept
{
  return _parentHost->modify_fd(fd, flags);
}
bool Plugin::posixFdSupportUnregisterFd(int fd) noexcept
{
  return _parentHost->unregister_fd(fd);
}
#endif
}  // namespace Clap
