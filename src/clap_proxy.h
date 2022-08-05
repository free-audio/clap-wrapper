#pragma once
#include <clap/clap.h>
#include <vector>
#include <string>
#include <memory>
#if WIN
#include <Windows.h>
#endif
#include "detail//clap/fsutil.h"

namespace Clap
{
  class Plugin;

  class IHost
  {
  public:
    virtual void mark_dirty() = 0;
    virtual void schnick() = 0;

    virtual void setupAudioBusses(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* audioports) = 0;   // called from initialize() to allow the setup of audio ports
    virtual void setupMIDIBusses(const clap_plugin_t* plugin, const clap_plugin_note_ports_t* noteports) = 0;      // called from initialize() to allow the setup of MIDI ports
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
    const clap_plugin_audio_ports_t* _audioports = nullptr;
    const clap_plugin_gui_t* _gui = nullptr;
    const clap_plugin_note_ports_t* _noteports = nullptr;
    const clap_plugin_midi_mappings_t* _midimap = nullptr;
    const clap_plugin_latency_t* _latency = nullptr;
    const clap_plugin_render_t* _render = nullptr;
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
  private:
    
    static const void* clapExtension(const clap_host* host, const char* extension);
    static void clapRequestCallback(const clap_host* host);
    static void clapRequestRestart(const clap_host* host);
    static void clapRequestProcess(const clap_host* host);

    //static bool clapIsMainThread(const clap_host* host);
    //static bool clapIsAudioThread(const clap_host* host);



    clap_host_t _host;                        // the host_t structure for the proxy
    IHost* _parentHost = nullptr;
    AudioSetup _audioSetup;
    bool _activated = false;
  };
}