#include "clap_proxy.h"
#include "AAX_CEffectParameters.h"
#include "AAX_CEffectGUI.h"
#include "AAX_ICollection.h"
#include "detail/aax/process.h"
#include "detail/aax/plugview.h"

// Context structure
struct SClapAsAAX_Alg_Context
{
  float** mInputPP;      // Audio signal destination
  float** mOutputPP;     // Audio signal source
  int32_t* mBufferSize;  // Buffer size
};

class ClapAsAAX : public Clap::IHost, public AAX_CEffectParameters, public AAX_CEffectGUI
{
 public:
  ClapAsAAX();
  ~ClapAsAAX() override;

  //---AAX Description API----------------------------------------------------------------

  // The plug-in's static description entrypoint.
  // This function is responsible for describing an AAX plug-in to the host. It does this by populating an AAX_ICollection interface.
  // This function must be included in every plug-in that links to the AAX library. It is called when the host first loads the plug-in.
  AAX_Result GetEffectDescriptions(AAX_ICollection* inCollection);

  //---AAX Data Model Interface-----------------------------------------------------------

  static AAX_CEffectParameters* AAX_CALLBACK Create();

  // AAX_CEffectParameters overrides

  AAX_Result EffectInit() AAX_OVERRIDE;

  //---AAX Algorithm Callback-------------------------------------------------------------
  void AAX_CALLBACK AlgorithmProcessFunction(SClapAsAAX_Alg_Context* const inInstancesBegin[],
                                             const void* inInstancesEnd);

  //---AAX GUI----------------------------------------------------------------------------
  void CreateViewContents() override;
  void CreateViewContainer() override;
  void DeleteViewContainer() override;

  //---Clap::IHost------------------------------------------------------------------------

  void setupWrapperSpecifics(const clap_plugin_t* plugin) override;

  void setupAudioBusses(const clap_plugin_t* plugin,
                        const clap_plugin_audio_ports_t* audioports) override;
  void setupMIDIBusses(const clap_plugin_t* plugin, const clap_plugin_note_ports_t* noteports) override;
  void setupParameters(const clap_plugin_t* plugin, const clap_plugin_params_t* params) override;

  void param_rescan(clap_param_rescan_flags flags) override;
  void param_clear(clap_id param, clap_param_clear_flags flags) override;
  void param_request_flush() override;

  bool gui_can_resize() override;
  bool gui_request_resize(uint32_t width, uint32_t height) override;
  bool gui_request_show() override;
  bool gui_request_hide() override;

  void latency_changed() override;

  void tail_changed() override;

  void mark_dirty() override;

  void restartPlugin() override;

  void request_callback() override;

  // clap_timer support
  bool register_timer(uint32_t period_ms, clap_id* timer_id) override;
  bool unregister_timer(clap_id timer_id) override;

  const char* host_get_name() override;

  bool supportsContextMenu() const override;
  // context_menu
  bool context_menu_populate(const clap_context_menu_target_t* target,
                             const clap_context_menu_builder_t* builder) override;
  bool context_menu_perform(const clap_context_menu_target_t* target, clap_id action_id) override;
  bool context_menu_can_popup() override;
  bool context_menu_popup(const clap_context_menu_target_t* target, int32_t screen_index, int32_t x,
                          int32_t y) override;

 private:
  Clap::Library* _library = nullptr;
  int _libraryIndex = 0;
  std::shared_ptr<Clap::Plugin> _plugin;
  Clap::AAX::ProcessAdapter* _processAdapter = nullptr;
  WrappedViewAAX* _wrappedView = nullptr;
};
