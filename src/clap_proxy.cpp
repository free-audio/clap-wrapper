#include "clap_proxy.h"
#include "detail/clap/fsutil.h"
#include "public.sdk/source/main/pluginfactory.h"

#if MAC || LIN
#include <iostream>
#define OutputDebugString(x) std::cout << __FILE__ << ":" << __LINE__ << " " << x << std::endl;
#endif

namespace Clap
{
  namespace HostExt
  {
    static Plugin* self(const clap_host_t* host)
    {
      return static_cast<Plugin*>(host->host_data);
    }

    void host_log(const clap_host_t* host, clap_log_severity severity, const char* msg)
    {
      self(host)->log(severity, msg);
    }

    clap_host_log_t log =
    {
      host_log
    };

    void rescan(const clap_host_t* host, clap_param_rescan_flags flags)
    {

    }

    // Clears references to a parameter.
    // [main-thread]
    void clear(const clap_host_t* host, clap_id param_id, clap_param_clear_flags flags)
    {

    }

    // Request a parameter flush.
    //
    // If the plugin is processing, this will result in no action. The process call
    // will run normally. If plugin isn't processing, the host will make a subsequent
    // call to clap_plugin_params->flush(). As a result, this function is always
    // safe to call from a non-audio thread (typically the UI thread on a gesture)
    // whether processing is active or not.
    //
    // This must not be called on the [audio-thread].
    // [thread-safe,!audio-thread]
    void request_flush(const clap_host_t* host)
    {

    }
    clap_host_params_t params =
    {
      rescan, clear, request_flush
    };

    bool is_main_thread(const clap_host_t* host)
    {
      return self(host)->is_main_thread();
    }

    // Returns true if "this" thread is one of the audio threads.
    // [thread-safe]
    bool is_audio_thread(const clap_host_t* host)
    {
      return self(host)->is_audio_thread();
    }

    clap_host_thread_check_t threadcheck =
    {
      is_main_thread, is_audio_thread
    };

  }

  class Raise
  {
  public:
    Raise(std::atomic<uint32_t>& counter)
      : ctx(counter)
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

  std::shared_ptr<Plugin> Plugin::createInstance(Clap::Library& library, size_t index, IHost* host)
  {
    if (library.plugins.size() > index)
    {
      auto plug = std::shared_ptr<Plugin>(new Plugin(host));
      auto instance = library._pluginFactory->create_plugin(library._pluginFactory, plug->getClapHostInterface(), library.plugins[index]->id);
      plug->connectClap(instance);

      return plug;
    }
    return nullptr;
  }
  Plugin::Plugin(IHost* host)
    : _host{
          CLAP_VERSION,
          this,
          "Clap-As-VST3-Wrapper",
          "defiant nerd",
          "https://www.defiantnerd.com",
          "0.0.1",
          Plugin::clapExtension,
          Plugin::clapRequestRestart,
          Plugin::clapRequestProcess,
          Plugin::clapRequestCallback
  }
    , _parentHost(host)
  {

  }

  template<typename T>
  void getExtension(const clap_plugin_t* plugin, T& ref, const char* name)
  {
    ref = static_cast<T>(plugin->get_extension(plugin, name));
  }

  void Plugin::connectClap(const clap_plugin_t* clap)
  {
    _plugin = clap;

    // initialize the plugin
    if (!_plugin->init(_plugin))
    {
      _plugin->destroy(_plugin);
      _plugin = nullptr;
      return;
    }

    // if successful, query the extensions the wrapper might want to use
    getExtension(_plugin, _ext._state, CLAP_EXT_STATE);
    getExtension(_plugin, _ext._params, CLAP_EXT_PARAMS);
    getExtension(_plugin, _ext._audioports, CLAP_EXT_AUDIO_PORTS);
    getExtension(_plugin, _ext._noteports, CLAP_EXT_NOTE_PORTS);
    getExtension(_plugin, _ext._midimap, CLAP_EXT_MIDI_MAPPINGS);
    getExtension(_plugin, _ext._latency, CLAP_EXT_LATENCY);
    getExtension(_plugin, _ext._render, CLAP_EXT_RENDER);
    getExtension(_plugin, _ext._gui, CLAP_EXT_GUI);

    if (_ext._gui)
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

      if (!_ext._gui->is_api_supported(_plugin, api, false))
      {
        // disable GUI if not win32
        _ext._gui = nullptr;
      }
    }
  }

  Plugin::~Plugin()
  {
    if (_plugin)
    {
      _plugin->destroy(_plugin);
      _plugin = nullptr;
    }

  }

  void Plugin::schnick()
  {

  }


  bool Plugin::initialize()
  {
    fprintf(stderr, "Plugin Initialize\n");
    if (_ext._audioports)
    {
      _parentHost->setupAudioBusses(_plugin, _ext._audioports);
    }
    if (_ext._noteports)
    {
      _parentHost->setupMIDIBusses(_plugin, _ext._noteports);
    }
    if (_ext._params)
    {
      _parentHost->setupParameters(_plugin, _ext._params);
    }
    return true;
  }

  void Plugin::terminate()
  {
    _plugin->destroy(_plugin);
    _plugin = nullptr;
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

  bool Plugin::load(const clap_istream_t* stream)
  {
    return false;
    if (_ext._state)
    {
      return _ext._state->load(_plugin, stream);
    }
    return false;
  }

  bool Plugin::save(const clap_ostream_t* stream)
  {
    if (_ext._state)
    {
      return _ext._state->save(_plugin, stream);
    }
    return false;
  }

  bool Plugin::activate()
  {
    return _plugin->activate(_plugin, _audioSetup.sampleRate, _audioSetup.minFrames, _audioSetup.maxFrames);
  }

  void Plugin::deactivate()
  {
    _plugin->deactivate(_plugin);
  }

  bool Plugin::start_processing()
  {
    auto thisFn = AlwaysAudioThread();
    return _plugin->start_processing(_plugin);
  }

  void Plugin::stop_processing()
  {
    auto thisFn = AlwaysAudioThread();
    _plugin->stop_processing(_plugin);
  }

  void Plugin::process(const clap_process_t* data)
  {
    auto thisFn = AlwaysAudioThread();
    _plugin->process(_plugin, data);
  }

  const clap_plugin_gui_t* Plugin::getUI()
  {
    if (_ext._gui)
    {
      if (_ext._gui->is_api_supported(_plugin, CLAP_WINDOW_API_WIN32, false))
      {
        // _ext._g
      }
    }
    return nullptr;
  }

  void Plugin::log(clap_log_severity severity, const char* msg)
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
#if WIN32
      OutputDebugString(msg);
      _CrtDbgBreak();
#endif
      break;
    }
    n.append(msg);
#if WIN32
    OutputDebugString(n.c_str());
    OutputDebugString("\n");
#endif
  }

  bool Plugin::is_main_thread() const
  {
    return _main_thread_id == std::this_thread::get_id();
  }

  bool Plugin::is_audio_thread() const
  {
    if (this->_audio_thread_override > 0)
    {
      return true;
    }
    return !is_main_thread();
  }

  CLAP_NODISCARD Raise Plugin::AlwaysAudioThread()
  {
      return Raise(this->_audio_thread_override); 
  }

  // Query an extension.
  // [thread-safe]
  const void* Plugin::clapExtension(const clap_host* host, const char* extension)
  {
    Plugin* self = reinterpret_cast<Plugin*>(host->host_data);

    OutputDebugString(extension); OutputDebugString("\n");
    if (!strcmp(extension, CLAP_EXT_LOG)) 
      return &HostExt::log;
    if (!strcmp(extension, CLAP_EXT_PARAMS)) 
      return &HostExt::params;
    if (!strcmp(extension, CLAP_EXT_THREAD_CHECK))
    {
      return &HostExt::threadcheck;
    }

    return nullptr;
  }

  // Request the host to schedule a call to plugin->on_main_thread(plugin) on the main thread.
  // [thread-safe]
  void Plugin::clapRequestCallback(const clap_host* host)
  {
    // this->scheduleUIThread();
  }

  // Request the host to deactivate and then reactivate the plugin.
  // The operation may be delayed by the host.
  // [thread-safe]
  void Plugin::clapRequestRestart(const clap_host* host)
  {
    auto self = static_cast<Plugin*>(host->host_data);
    self->_parentHost->schnick();
  }

  // Request the host to activate and start processing the plugin.
  // This is useful if you have external IO and need to wake up the plugin from "sleep".
  // [thread-safe]
  void Plugin::clapRequestProcess(const clap_host* host)
  {
    // right now, I don't know how to communicate this to the host
  }

}
