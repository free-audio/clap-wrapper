#pragma once

/*
    CLAP Proxy

    Copyright (c) 2022 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    The CLAP Proxy class interfaces the actual CLAP plugin and the serves the IHost interface which
    is implemented by the different wrapper flavors.

*/

#include <clap/clap.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>

#if WIN
#include <windows.h>
#endif

#include "detail/clap/fsutil.h"
#include <clap/helpers/host.hh>
#include <clap/helpers/plugin-proxy.hh>

namespace Clap
{
constexpr auto Plugin_MH = clap::helpers::MisbehaviourHandler::Ignore;
constexpr auto Plugin_CL = clap::helpers::CheckingLevel::None;

using PluginHostBase = clap::helpers::Host<Plugin_MH, Plugin_CL>;
using PluginProxy = clap::helpers::PluginProxy<Plugin_MH, Plugin_CL>;
}  // namespace Clap

extern template class clap::helpers::Host<Clap::Plugin_MH, Clap::Plugin_CL>;
extern template class clap::helpers::PluginProxy<Clap::Plugin_MH, Clap::Plugin_CL>;

namespace Clap
{
class Plugin;
class Raise;

// the IHost interface is being implemented by the actual wrapper class
class IHost
{
 public:
  virtual void mark_dirty() = 0;
  virtual void restartPlugin() = 0;
  virtual void request_callback() = 0;

  virtual void setupWrapperSpecifics(
      const PluginProxy& plugin) = 0;  // called when a wrapper could scan for wrapper specific plugins

  virtual void setupAudioBusses(
      const PluginProxy& proxy) = 0;  // called from initialize() to allow the setup of audio ports
  virtual void setupMIDIBusses(
      const PluginProxy& proxy) = 0;  // called from initialize() to allow the setup of MIDI ports
  virtual void setupParameters(const PluginProxy& proxy) = 0;

  virtual void param_rescan(clap_param_rescan_flags flags) = 0;  // ext_host_params
  virtual void param_clear(clap_id param, clap_param_clear_flags flags) = 0;
  virtual void param_request_flush() = 0;

  virtual void latency_changed() = 0;
  virtual void tail_changed() = 0;

  virtual bool gui_can_resize() = 0;
  virtual bool gui_request_resize(uint32_t width, uint32_t height) = 0;
  virtual bool gui_request_show() = 0;
  virtual bool gui_request_hide() = 0;

  virtual bool register_timer(uint32_t period_ms, clap_id* timer_id) = 0;
  virtual bool unregister_timer(clap_id timer_id) = 0;

#if LIN
  virtual bool register_fd(int fd, clap_posix_fd_flags_t flags) = 0;
  virtual bool modify_fd(int fd, clap_posix_fd_flags_t flags) = 0;
  virtual bool unregister_fd(int fd) = 0;
#endif
};

struct AudioSetup
{
  double sampleRate = 44100.;
  uint32_t minFrames = 0;
  uint32_t maxFrames = 0;
};

class Raise
{
 public:
  Raise(std::atomic<uint32_t>& counter) : ctx(counter)
  {
    ++ctx;
  }
  ~Raise()
  {
    ctx--;
  }

 private:
  std::atomic<uint32_t>& ctx;
};

/// <summary>
/// Plugin is the `host` for the CLAP plugin instance
/// and the interface for the VST3 plugin wrapper
/// </summary>
class Plugin : public Clap::PluginHostBase
{
 public:
  static std::shared_ptr<Plugin> createInstance(const clap_plugin_factory*, const std::string& id,
                                                IHost* host);
  static std::shared_ptr<Plugin> createInstance(const clap_plugin_factory*, size_t idx, IHost* host);
  static std::shared_ptr<Plugin> createInstance(Clap::Library& library, size_t index, IHost* host);

 protected:
  // only the Clap::Library is allowed to create instances
  Plugin(IHost* host);
  void connectClap(const clap_plugin_t* clap);

 public:
  Plugin(const Plugin&) = delete;
  Plugin(Plugin&&) = delete;
  Plugin& operator=(const Plugin&) = delete;
  Plugin& operator=(Plugin&&) = delete;
  ~Plugin();

  void schnick();
  bool initialize();
  void terminate();
  void setSampleRate(double sampleRate);
  double getSampleRate() const
  {
    return _audioSetup.sampleRate;
  }
  void setBlockSizes(uint32_t minFrames, uint32_t maxFrames);

  bool load(const clap_istream_t* stream) const;
  bool save(const clap_ostream_t* stream) const;
  bool activate() const;
  void deactivate() const;
  bool start_processing();
  void stop_processing();
  // void process(const clap_process_t* data);
  const clap_plugin_gui_t* getUI() const;

  PluginProxy* getProxy() const;

  CLAP_NODISCARD Raise AlwaysAudioThread();
  CLAP_NODISCARD Raise AlwaysMainThread();

 protected:
  ////////////////////
  // PluginHostBase //
  ////////////////////

  // clap_host
  void requestRestart() noexcept override;
  void requestProcess() noexcept override;
  void requestCallback() noexcept override;

  // clap_host_gui
  bool implementsGui() const noexcept override
  {
    return true;
  }
  void guiResizeHintsChanged() noexcept override
  {
  }
  bool guiRequestResize(uint32_t width, uint32_t height) noexcept override
  {
    if (!_parentHost->gui_can_resize()) return false;

    _parentHost->gui_request_resize(width, height);
    return true;
  }
  bool guiRequestShow() noexcept override
  {
    return _parentHost->gui_request_show();
  }
  bool guiRequestHide() noexcept override
  {
    return false;
  }
  void guiClosed(bool /*wasDestroyed*/) noexcept override
  {
  }

  // clap_host_latency
  bool implementsLatency() const noexcept override
  {
    return true;
  }
  void latencyChanged() noexcept override;

  // clap_host_log
  bool implementsLog() const noexcept override
  {
    return true;
  }
  void logLog(clap_log_severity severity, const char* message) const noexcept override;

  // clap_host_params
  bool implementsParams() const noexcept override
  {
    return true;
  }
  void paramsRescan(clap_param_rescan_flags flags) noexcept override;
  void paramsClear(clap_id paramId, clap_param_clear_flags flags) noexcept override;
  void paramsRequestFlush() noexcept override;

#if LIN
  // clap_host_posix_fd_support
  bool implementsPosixFdSupport() const noexcept override
  {
    return true;
  }
  bool posixFdSupportRegisterFd(int fd, clap_posix_fd_flags_t flags) noexcept override;
  bool posixFdSupportModifyFd(int fd, clap_posix_fd_flags_t flags) noexcept override;
  bool posixFdSupportUnregisterFd(int fd) noexcept override;
#endif

  // clap_host_tail
  bool implementsTail() const noexcept override
  {
    return true;
  }
  void tailChanged() noexcept override;

  // clap_host_timer_support
  bool implementsTimerSupport() const noexcept override
  {
    return true;
  }
  bool timerSupportRegisterTimer(uint32_t periodMs, clap_id* timerId) noexcept override;
  bool timerSupportUnregisterTimer(clap_id timerId) noexcept override;

  // clap_host_thread_check
  bool threadCheckIsMainThread() const noexcept override;
  bool threadCheckIsAudioThread() const noexcept override;

 private:
  IHost* _parentHost = nullptr;
  std::unique_ptr<PluginProxy> _proxy;
  const std::thread::id _main_thread_id = std::this_thread::get_id();
  std::atomic<uint32_t> _audio_thread_override = 0;
  std::atomic<uint32_t> _main_thread_override = 0;

  AudioSetup _audioSetup;
};

class StateMemento
{
 public:
  void clear()
  {
    _mem.clear();
  }
  void rewind()
  {
    _readoffset = 0;
  }
  const uint8_t* data()
  {
    return _mem.data();
  }
  size_t size()
  {
    return _mem.size();
  }
  void setData(const uint8_t* data, size_t size);
  operator const clap_ostream_t*()
  {
    return &_outstream;
  }
  operator const clap_istream_t*()
  {
    return &_instream;
  }

 private:
  uint64_t _readoffset = 0;
  static int64_t CLAP_ABI _read(const struct clap_istream* stream, void* buffer, uint64_t size);
  static int64_t CLAP_ABI _write(const struct clap_ostream* stream, const void* buffer, uint64_t size);
  clap_ostream_t _outstream = {this, _write};
  clap_istream_t _instream = {this, _read};
  std::vector<uint8_t> _mem;
};
}  // namespace Clap
