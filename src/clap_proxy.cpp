#include "clap_proxy.h"
#include "detail/clap/fsutil.h"
#include "public.sdk/source/main/pluginfactory.h"

namespace Clap
{
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
      if (!_ext._gui->is_api_supported(_plugin, CLAP_WINDOW_API_WIN32, false))
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
    return _plugin->start_processing(_plugin);
  }

  void Plugin::stop_processing()
  {
    _plugin->stop_processing(_plugin);
  }

  void Plugin::process(const clap_process_t* data)
  {
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

  // Query an extension.
  // [thread-safe]
  const void* Plugin::clapExtension(const clap_host* host, const char* extension)
  {
    Plugin* self = reinterpret_cast<Plugin*>(host->host_data);
    // OutputDebugString(extension); OutputDebugString("\n");
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
