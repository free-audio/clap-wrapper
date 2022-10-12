#pragma once
#include <clap/clap.h>
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>

#if WIN
#include <Windows.h>
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
    virtual void mark_dirty() = 0;
    virtual void schnick() = 0;
    virtual void request_callback() = 0;

    virtual void setupWrapperSpecifics(const clap_plugin_t* plugin) = 0;                                           // called when a wrapper could scan for wrapper specific plugins

    virtual void setupAudioBusses(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* audioports) = 0;   // called from initialize() to allow the setup of audio ports
    virtual void setupMIDIBusses(const clap_plugin_t* plugin, const clap_plugin_note_ports_t* noteports) = 0;      // called from initialize() to allow the setup of MIDI ports
    virtual void setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params) = 0;

    virtual void param_rescan(clap_param_rescan_flags flags) = 0;                                                  // ext_host_params
    virtual void param_clear(clap_id param, clap_param_clear_flags flags) = 0;
    virtual void param_request_flush() = 0;

    virtual bool gui_can_resize() = 0;
    virtual bool gui_request_resize(uint32_t width, uint32_t height) = 0;
    virtual bool gui_request_show() = 0;
    virtual bool gui_request_hide() = 0;

    virtual bool register_timer(uint32_t period_ms, clap_id* timer_id) = 0;
    virtual bool unregister_timer(clap_id timer_id) = 0;

    virtual void latency_changed() = 0;

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
    const clap_plugin_midi_mappings_t* _midimap = nullptr;
    const clap_plugin_latency_t* _latency = nullptr;
    const clap_plugin_render_t* _render = nullptr;
    const clap_plugin_timer_support_t* _timer = nullptr;
  };

  /// <summary>
  /// Plugin is the `host` for the CLAP plugin instance
  /// and the interface for the VST3 plugin wrapper
  /// </summary>
  class Plugin
  {
  public:
    static std::shared_ptr<Plugin> createInstance(Clap::Library& library, size_t index, IHost* host);
  protected:
    // only the Clap::Library is allowed to create instances
    Plugin(IHost* host);
    const clap_host_t* getClapHostInterface() { return &_host; }
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
    void setBlockSizes(uint32_t minFrames, uint32_t maxFrames);

    bool load(const clap_istream_t* stream);
    bool save(const clap_ostream_t* stream);
    bool activate();
    void deactivate();
    bool start_processing();
    void stop_processing();
    void process(const clap_process_t* data);
    const clap_plugin_gui_t* getUI();

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

    // latency
    void latency_changed();

    // hostgui
    void resize_hints_changed()
    {
    }
    bool request_resize(uint32_t width, uint32_t height)
    {
      if (_parentHost->gui_can_resize())
      {
        _parentHost->gui_request_resize(width, height);
        return true;
      }
      return false;
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
    {    }

    // clap_timer support
    bool register_timer(uint32_t period_ms, clap_id* timer_id);
    bool unregister_timer(clap_id timer_id);
  private:
    CLAP_NODISCARD Raise AlwaysAudioThread();
    
    static const void* clapExtension(const clap_host* host, const char* extension);
    static void clapRequestCallback(const clap_host* host);
    static void clapRequestRestart(const clap_host* host);
    static void clapRequestProcess(const clap_host* host);

    //static bool clapIsMainThread(const clap_host* host);
    //static bool clapIsAudioThread(const clap_host* host);


    clap_host_t _host;                        // the host_t structure for the proxy
    IHost* _parentHost = nullptr;
    const std::thread::id _main_thread_id = std::this_thread::get_id();
    std::atomic<uint32_t> _audio_thread_override = 0;
    AudioSetup _audioSetup;
    bool _activated = false;
  };
}