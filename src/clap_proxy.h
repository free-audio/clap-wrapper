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

namespace Clap
{
class Plugin;
class Raise;

// the IHost interface is being implemented by the actual wrapper class
class IHost
{
 public:
  virtual ~IHost() = default;

  virtual void mark_dirty() = 0;
  virtual void restartPlugin() = 0;
  virtual void request_callback() = 0;

  virtual void setupWrapperSpecifics(
      const clap_plugin_t* plugin) = 0;  // called when a wrapper could scan for wrapper specific plugins

  virtual void setupAudioBusses(
      const clap_plugin_t* plugin,
      const clap_plugin_audio_ports_t*
          audioports) = 0;  // called from initialize() to allow the setup of audio ports
  virtual void setupMIDIBusses(
      const clap_plugin_t* plugin,
      const clap_plugin_note_ports_t*
          noteports) = 0;  // called from initialize() to allow the setup of MIDI ports
  virtual void setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params) = 0;

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

  virtual const char* host_get_name() = 0;

  // context menu

  // actually, everything here should be virtual only, but until all wrappers are updated,
  // IHost provides default implementations.

  virtual bool supportsContextMenu() const = 0;
  virtual bool context_menu_populate(const clap_context_menu_target_t* target,
                                     const clap_context_menu_builder_t* builder) = 0;
  virtual bool context_menu_perform(const clap_context_menu_target_t* target, clap_id action_id) = 0;
  virtual bool context_menu_can_popup() = 0;
  virtual bool context_menu_popup(const clap_context_menu_target_t* target, int32_t screen_index,
                                  int32_t x, int32_t y) = 0;

#if LIN
  virtual bool register_fd(int fd, clap_posix_fd_flags_t flags) = 0;
  virtual bool modify_fd(int fd, clap_posix_fd_flags_t flags) = 0;
  virtual bool unregister_fd(int fd) = 0;
#endif
};

struct ClapPluginExtensions;

struct AudioSetup
{
  double sampleRate = 44100.;
  uint32_t minFrames = 0;
  uint32_t maxFrames = 0;
};

struct ClapPluginExtensions
{
  const clap_plugin_state_t* _state = nullptr;
  const clap_plugin_params_t* _params = nullptr;
  const clap_plugin_audio_ports_t* _audioports = nullptr;
  const clap_plugin_gui_t* _gui = nullptr;
  const clap_plugin_note_ports_t* _noteports = nullptr;
  const clap_plugin_latency_t* _latency = nullptr;
  const clap_plugin_render_t* _render = nullptr;
  const clap_plugin_tail_t* _tail = nullptr;
  const clap_plugin_timer_support_t* _timer = nullptr;
  const clap_plugin_context_menu_t* _contextmenu = nullptr;
  const clap_ara_plugin_extension_t* _ara = nullptr;
#if LIN
  const clap_plugin_posix_fd_support* _posixfd = nullptr;
#endif
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
class Plugin
{
 public:
  static std::shared_ptr<Plugin> createInstance(const clap_plugin_factory*, const std::string& id,
                                                IHost* host);
  static std::shared_ptr<Plugin> createInstance(const clap_plugin_factory*, size_t idx, IHost* host);
  static std::shared_ptr<Plugin> createInstance(Clap::Library& library, size_t index, IHost* host);

 protected:
  // only the Clap::Library is allowed to create instances
  Plugin(IHost* host);
  const clap_host_t* getClapHostInterface()
  {
    return &_host;
  }
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

  ClapPluginExtensions _ext;
  const clap_plugin_t* _plugin = nullptr;
  void log(clap_log_severity severity, const char* msg);

  // threadcheck
  bool is_main_thread() const;
  bool is_audio_thread() const;

  // param
  void param_rescan(clap_param_rescan_flags flags);
  void param_clear(clap_id param, clap_param_clear_flags flags);
  void param_request_flush();

  // state
  void mark_dirty();

  // latency
  void latency_changed();

  // tail
  void tail_changed();

  // context_menu
  bool context_menu_populate(const clap_context_menu_target_t* target,
                             const clap_context_menu_builder_t* builder);
  bool context_menu_perform(const clap_context_menu_target_t* target, clap_id action_id);
  bool context_menu_can_popup();
  bool context_menu_popup(const clap_context_menu_target_t* target, int32_t screen_index, int32_t x,
                          int32_t y);

  // hostgui
  void resize_hints_changed()
  {
  }
  bool request_resize(uint32_t width, uint32_t height)
  {
    return _parentHost->gui_request_resize(width, height);
  }
  bool request_show()
  {
    return _parentHost->gui_request_show();
  }
  bool request_hide()
  {
    return false;
  }
  void closed(bool was_destroyed)
  {
  }

  // clap_timer support
  bool register_timer(uint32_t period_ms, clap_id* timer_id);
  bool unregister_timer(clap_id timer_id);

#if LIN
  bool register_fd(int fd, clap_posix_fd_flags_t flags);
  bool modify_fd(int fd, clap_posix_fd_flags_t flags);
  bool unregister_fd(int fd);

#endif
  CLAP_NODISCARD Raise AlwaysAudioThread();
  CLAP_NODISCARD Raise AlwaysMainThread();

 private:
  static const void* clapExtension(const clap_host* host, const char* extension);
  static void clapRequestCallback(const clap_host* host);
  static void clapRequestRestart(const clap_host* host);
  static void clapRequestProcess(const clap_host* host);

  //static bool clapIsMainThread(const clap_host* host);
  //static bool clapIsAudioThread(const clap_host* host);

  clap_host_t _host;  // the host_t structure for the proxy
  IHost* _parentHost = nullptr;
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
