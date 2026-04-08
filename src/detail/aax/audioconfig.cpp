#include "audioconfig.h"

#include "AAX_IEffectDescriptor.h"
#include "AAX_IComponentDescriptor.h"
#include "AAX_IPropertyMap.h"

namespace CLAPAAX
{
// clang-format off

  // static maps of channel mappings for CLAP surround from AAX STEM formats
  // the order is defined by the AAX stem channel format, see AAX_Enums.h for "enum AAX_EStemFormat"
  //
  // there are a few assumptions here, since AAX does not completely map with this
  // 
  // ** Ls/Rs in 5.1/7.1
  // mapped to consistently to "Side", so Lss/Rss -> SL/SR and Lsr/Rsr -> BL/BR
  //
  // ** Lw/Rw ("Wide")
  // CLAP does not have Wide-Channels, so they will go to front, so Lw/Rw -> FLC/FRC. This seems to be a common pattern.
  //
  // ** Top Middle (Ltm/Rtm)
  // mapped to their CLAP equivalents (TFL/TFR/TSL/TSR/TBL/TBR/TC/TFC/TBC).
  //
  // additional note: if you, dearest reader, find any issue with the following definition,
  // please open a github issue, this is a "there be dragons" area for me

  // AAX -> CLAP channel constants
#define AAX_M    CLAP_SURROUND_FC
#define AAX_L    CLAP_SURROUND_FL
#define AAX_R    CLAP_SURROUND_FR
#define AAX_C    CLAP_SURROUND_FC
#define AAX_LFE  CLAP_SURROUND_LFE
#define AAX_Lc   CLAP_SURROUND_FLC
#define AAX_Rc   CLAP_SURROUND_FRC
#define AAX_Ls   CLAP_SURROUND_SL
#define AAX_Rs   CLAP_SURROUND_SR
#define AAX_Lss  CLAP_SURROUND_SL
#define AAX_Rss  CLAP_SURROUND_SR
#define AAX_Lsr  CLAP_SURROUND_BL
#define AAX_Rsr  CLAP_SURROUND_BR
#define AAX_S    CLAP_SURROUND_BC
#define AAX_Cs   CLAP_SURROUND_BC
#define AAX_Lw   CLAP_SURROUND_FLC
#define AAX_Rw   CLAP_SURROUND_FRC

// Top / Height channels
#define AAX_Ltf  CLAP_SURROUND_TFL
#define AAX_Rtf  CLAP_SURROUND_TFR
#define AAX_Ltm  CLAP_SURROUND_TSL
#define AAX_Rtm  CLAP_SURROUND_TSR
#define AAX_Ltr  CLAP_SURROUND_TBL
#define AAX_Rtr  CLAP_SURROUND_TBR
#define AAX_Lts  CLAP_SURROUND_TSL
#define AAX_Rts  CLAP_SURROUND_TSR
#define AAX_TC   CLAP_SURROUND_TC
#define AAX_TFC  CLAP_SURROUND_TFC
#define AAX_TBC  CLAP_SURROUND_TBC

  static const uint8_t aax2clap_Mono[] = { AAX_M };
  static const uint8_t aax2clap_Stereo[] = { AAX_L,                              AAX_R };
  static const uint8_t aax2clap_LCR[] = { AAX_L,         AAX_C,               AAX_R };
  static const uint8_t aax2clap_LCRS[] = { AAX_L,         AAX_C,               AAX_R, AAX_S };
  static const uint8_t aax2clap_Quad[] = { AAX_L,                              AAX_R, AAX_Ls, AAX_Rs };
  static const uint8_t aax2clap_5_0[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Rs };
  static const uint8_t aax2clap_5_1[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Rs, AAX_LFE };
  static const uint8_t aax2clap_6_0[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Cs, AAX_Rs };
  static const uint8_t aax2clap_6_1[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Cs, AAX_Rs, AAX_LFE };
  static const uint8_t aax2clap_7_0_SDDS[] = { AAX_L, AAX_Lc, AAX_C, AAX_Rc,       AAX_R, AAX_Ls, AAX_Rs };
  static const uint8_t aax2clap_7_1_SDDS[] = { AAX_L, AAX_Lc, AAX_C, AAX_Rc,       AAX_R, AAX_Ls, AAX_Rs, AAX_LFE };
  static const uint8_t aax2clap_7_0_DTS[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr };
  static const uint8_t aax2clap_7_1_DTS[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_LFE };
  static const uint8_t aax2clap_7_0_2[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_Lts, AAX_Rts };
  static const uint8_t aax2clap_7_1_2[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_LFE, AAX_Lts, AAX_Rts };
  static const uint8_t aax2clap_5_0_2[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Rs, AAX_Ltm, AAX_Rtm };
  static const uint8_t aax2clap_5_1_2[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Rs, AAX_LFE, AAX_Ltm, AAX_Rtm };
  static const uint8_t aax2clap_5_0_4[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Rs, AAX_Ltf, AAX_Rtf, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_5_1_4[] = { AAX_L,         AAX_C,               AAX_R, AAX_Ls, AAX_Rs, AAX_LFE, AAX_Ltf, AAX_Rtf, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_7_0_4[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_Ltf, AAX_Rtf, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_7_1_4[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_LFE, AAX_Ltf, AAX_Rtf, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_7_0_6[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_Ltf, AAX_Rtf, AAX_Ltm, AAX_Rtm, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_7_1_6[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_LFE, AAX_Ltf, AAX_Rtf, AAX_Ltm, AAX_Rtm, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_9_0_4[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lw, AAX_Rw, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_Ltf, AAX_Rtf, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_9_1_4[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lw, AAX_Rw, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_LFE, AAX_Ltf, AAX_Rtf, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_9_0_6[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lw, AAX_Rw, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_Ltf, AAX_Rtf, AAX_Ltm, AAX_Rtm, AAX_Ltr, AAX_Rtr };
  static const uint8_t aax2clap_9_1_6[] = { AAX_L,         AAX_C,               AAX_R, AAX_Lw, AAX_Rw, AAX_Lss, AAX_Rss, AAX_Lsr, AAX_Rsr, AAX_LFE, AAX_Ltf, AAX_Rtf, AAX_Ltm, AAX_Rtm, AAX_Ltr, AAX_Rtr };

#undef AAX_M 
#undef AAX_L 
#undef AAX_R 
#undef AAX_C 
#undef AAX_LF
#undef AAX_Lc
#undef AAX_Rc
#undef AAX_Ls
#undef AAX_Rs
#undef AAX_Ls
#undef AAX_Rs
#undef AAX_Ls
#undef AAX_Rs
#undef AAX_S 
#undef AAX_Cs
#undef AAX_Lw
#undef AAX_Rw

#undef AAX_Ltf 
#undef AAX_Rtf 
#undef AAX_Ltm 
#undef AAX_Rtm 
#undef AAX_Ltr 
#undef AAX_Rtr 
#undef AAX_Lts 
#undef AAX_Rts 
#undef AAX_TC  
#undef AAX_TFC 
#undef AAX_TBC

// clang-format on

static sAAXStemIndexToClapMap_t aaxchannelmaps[] = {
    {"Mono", AAX_eStemFormat_Mono, aax2clap_Mono, sizeof(aax2clap_Mono)},
    {"Stereo", AAX_eStemFormat_Stereo, aax2clap_Stereo, sizeof(aax2clap_Stereo)},
    {"LCR", AAX_eStemFormat_LCR, aax2clap_LCR, sizeof(aax2clap_LCR)},
    {"LCRS", AAX_eStemFormat_LCRS, aax2clap_LCRS, sizeof(aax2clap_LCRS)},
    {"Quad", AAX_eStemFormat_Quad, aax2clap_Quad, sizeof(aax2clap_Quad)},
    {"5_0", AAX_eStemFormat_5_0, aax2clap_5_0, sizeof(aax2clap_5_0)},
    {"5_1", AAX_eStemFormat_5_1, aax2clap_5_1, sizeof(aax2clap_5_1)},
    {"6_0", AAX_eStemFormat_6_0, aax2clap_6_0, sizeof(aax2clap_6_0)},
    {"6_1", AAX_eStemFormat_6_1, aax2clap_6_1, sizeof(aax2clap_6_1)},
    {"7_0_SDDS", AAX_eStemFormat_7_0_SDDS, aax2clap_7_0_SDDS, sizeof(aax2clap_7_0_SDDS)},
    {"7_1_SDDS", AAX_eStemFormat_7_1_SDDS, aax2clap_7_1_SDDS, sizeof(aax2clap_7_1_SDDS)},
    {"7_0_DTS", AAX_eStemFormat_7_0_DTS, aax2clap_7_0_DTS, sizeof(aax2clap_7_0_DTS)},
    {"7_1_DTS", AAX_eStemFormat_7_1_DTS, aax2clap_7_1_DTS, sizeof(aax2clap_7_1_DTS)},
    {"7_0_2", AAX_eStemFormat_7_0_2, aax2clap_7_0_2, sizeof(aax2clap_7_0_2)},
    {"7_1_2", AAX_eStemFormat_7_1_2, aax2clap_7_1_2, sizeof(aax2clap_7_1_2)},
    {"5_0_2", AAX_eStemFormat_5_0_2, aax2clap_5_0_2, sizeof(aax2clap_5_0_2)},
    {"5_1_2", AAX_eStemFormat_5_1_2, aax2clap_5_1_2, sizeof(aax2clap_5_1_2)},
    {"5_0_4", AAX_eStemFormat_5_0_4, aax2clap_5_0_4, sizeof(aax2clap_5_0_4)},
    {"5_1_4", AAX_eStemFormat_5_1_4, aax2clap_5_1_4, sizeof(aax2clap_5_1_4)},
    {"7_0_4", AAX_eStemFormat_7_0_4, aax2clap_7_0_4, sizeof(aax2clap_7_0_4)},
    {"7_1_4", AAX_eStemFormat_7_1_4, aax2clap_7_1_4, sizeof(aax2clap_7_1_4)},
    {"7_0_6", AAX_eStemFormat_7_0_6, aax2clap_7_0_6, sizeof(aax2clap_7_0_6)},
    {"7_1_6", AAX_eStemFormat_7_1_6, aax2clap_7_1_6, sizeof(aax2clap_7_1_6)},
    {"9_0_4", AAX_eStemFormat_9_0_4, aax2clap_9_0_4, sizeof(aax2clap_9_0_4)},
    {"9_1_4", AAX_eStemFormat_9_1_4, aax2clap_9_1_4, sizeof(aax2clap_9_1_4)},
    {"9_0_6", AAX_eStemFormat_9_0_6, aax2clap_9_0_6, sizeof(aax2clap_9_0_6)},
    {"9_1_6", AAX_eStemFormat_9_1_6, aax2clap_9_1_6, sizeof(aax2clap_9_1_6)},
};

// this function retrieves a list of available bus configurations that a plugin supports
// when the
plugin_bus_info_t getAvailableBusConfigs(Clap::Library *factory, uint32_t index)
{
  plugin_bus_info_t result;
  const auto pdesc = factory->plugins[index];

  // add an EffectDescription for each plugin available via factory
  const clap_plugin_info_as_aax_t *plug_aax_info = nullptr;
  if (factory->_pluginFactoryAAXInfo)
  {
    plug_aax_info = factory->_pluginFactoryAAXInfo->get_aax_info(factory->_pluginFactoryAAXInfo, index);
  }
  // -------------------------------------------------------------------------

  using configrequests_t = std::vector<clap_audio_port_configuration_request>;

  if (plug_aax_info)
  {
    uint32_t N = plug_aax_info->get_num_stem_configs();
    LOGINFO("retrieving {} configs from plugin '{}'", N, pdesc->id);
    for (uint32_t i = 0; i < N; ++i)
    {
      auto *steminfo = plug_aax_info->get_stem_config(i);
      result.stemformats.push_back(
          {steminfo->name, steminfo->format_in, steminfo->format_out, steminfo->plugin_id});
    }
    if (plug_aax_info->midi_in_name)
    {
      result.has_midi_in = true;
      result.midi_in_name = plug_aax_info->midi_in_name;
    }
    if (plug_aax_info->midi_out_name)
    {
      result.has_midi_out = true;
      result.midi_out_name = plug_aax_info->midi_out_name;
    }
    return result;
  }

  // the local microhost
  // why here? because we have factory and the clap-id
  static const clap_host_params_t micro_params = {
      [](const clap_host_t *host, clap_param_rescan_flags flags) -> void {},
      [](const clap_host_t *host, clap_id param_id, clap_param_clear_flags flags) -> void {},
      [](const clap_host_t *host) -> void {}};
  static const clap_host_audio_ports_t micro_audio_ports = {
      [](const clap_host_t *host, uint32_t flag) -> bool { return false; },
      [](const clap_host_t *host, uint32_t flags) -> void {}};
  clap_host_t microhost = {
      CLAP_VERSION,
      nullptr,
      "aax_scanner",
      "clap_wrapper",
      "",
      "1.0",
      [](const struct clap_host *host, const char *extension_id) -> const void *
      {
        if (extension_id == nullptr) return nullptr;
        LOGDETAIL("plugin requests microhost extension {}", extension_id);
        if (!strcmp(CLAP_EXT_PARAMS, extension_id)) return &micro_params;
        if (!strcmp(CLAP_EXT_AUDIO_PORTS, extension_id)) return &micro_audio_ports;
        return nullptr;
      },
      [](const struct clap_host *host) -> void {},  // request_restart
      [](const struct clap_host *host) -> void {},  // request_process
      [](const struct clap_host *host) -> void {},  // request_callback
  };

  try
  {
    // create a temporary plugin instance ------------------
    auto *tmpplug =
        factory->_pluginFactory->create_plugin(factory->_pluginFactory, &microhost, pdesc->id);
    try
    {
      tmpplug->init(tmpplug);
      auto ext_aud = (clap_plugin_audio_ports *)(tmpplug->get_extension(tmpplug, CLAP_EXT_AUDIO_PORTS));
      auto ext_cap = (clap_plugin_configurable_audio_ports_t *)(tmpplug->get_extension(
          tmpplug, CLAP_EXT_CONFIGURABLE_AUDIO_PORTS));
      auto ext_notes =
          (const clap_plugin_note_ports_t *)(tmpplug->get_extension(tmpplug, CLAP_EXT_NOTE_PORTS));

      // build a bus setting ------------------
      configrequests_t requests;
      // bool standardconfig_is_mono_or_stereo = true;

      if (ext_aud)
      {
        if (ext_cap)
        {
          // collect input and output definitions for each audio bus
          // build a structure that reflects those busses and re use
          // them the check of each stem formats.

          uint32_t numins = ext_aud->count(tmpplug, true);
          uint32_t numout = ext_aud->count(tmpplug, false);
          for (uint32_t i = 0; i < numins; ++i)
          {
            clap_audio_port_info_t info;
            if (ext_aud->get(tmpplug, i, true, &info))
            {
              // {true, 0, 1, CLAP_PORT_MONO, nullptr},
              requests.emplace_back(clap_audio_port_configuration_request{true, i, info.channel_count,
                                                                          info.port_type, nullptr});
            }
          }
          // collect output definition for each audio bus
          for (uint32_t i = 0; i < numout; ++i)
          {
            clap_audio_port_info_t info;
            if (ext_aud->get(tmpplug, i, true, &info))
            {
              // {false, 0, 1, CLAP_PORT_MONO, nullptr},
              requests.emplace_back(clap_audio_port_configuration_request{false, i, info.channel_count,
                                                                          info.port_type, nullptr});
            }
          }

          // the config array is set, now go through the stem formats and check
          // if their CLAP equivalents are valid.
          result.stemformats.clear();
          for (const auto &i : aaxchannelmaps)
          {
            // input and output have the same format
            for (auto &c : requests)
            {
              // c.port_index and c.is_input is already set, now apply channel count and (optionally) surround channel map
              c.channel_count = AAX_STEM_FORMAT_CHANNEL_COUNT(i.aaxStemformat);
              switch (AAX_STEM_FORMAT_CHANNEL_COUNT(i.aaxStemformat))
              {
                case 1:
                  c.port_type = CLAP_PORT_MONO;
                  c.port_details = nullptr;
                  break;
                case 2:
                  c.port_type = CLAP_PORT_STEREO;
                  c.port_details = nullptr;
                  break;
                default:
                  // assert(c.channel_count == i.mapsize);
                  c.port_type = CLAP_PORT_SURROUND;
                  c.port_details = i.clapmap;
                  break;
              }
            }
            // now check if the plugin accepts this
            if (ext_cap->can_apply_configuration(tmpplug, &requests[0], (uint32_t)requests.size()))
            {
              std::string configname = fmt::format("{}/{}", i.aaxStemformat, i.aaxStemformat);

              // if yes, push it to the list of working configurations
              result.stemformats.push_back({configname, i.aaxStemformat, i.aaxStemformat});
            }
          }
        }
        else
        {
          // if not, we fall back to mono/stereo checks
          clap_audio_port_info_t p;
          uint32_t numinputs = ext_aud->count(tmpplug, true);
          uint32_t numoutputs = ext_aud->count(tmpplug, false);
          std::string f;
          uint32_t informat = 0, outformat = 0;
          if (numinputs > 0)
          {
            ext_aud->get(tmpplug, 0, false, &p);
            switch (p.channel_count)
            {
              case 1:
                informat = AAX_eStemFormat_Mono;
                f = "Mono/";
                break;
              case 2:
                informat = AAX_eStemFormat_Stereo;
                f = "Stereo/";
                break;
              default:
                break;
            }
          }
          if (numoutputs > 0)
          {
            ext_aud->get(tmpplug, 0, false, &p);
            switch (p.channel_count)
            {
              case 1:
                outformat = AAX_eStemFormat_Mono;
                f.append("Mono");
                break;
              case 2:
                outformat = AAX_eStemFormat_Stereo;
                f.append("Stereo");
                break;
              default:
                break;
            }
          }
          result.stemformats.push_back({f, informat, outformat});
        }
      }
      else
      {
        LOGINFO("no audio ports extension found");
      }

      // probe MIDI note port support
      if (ext_notes)
      {
        uint32_t n_in = ext_notes->count(tmpplug, true);
        for (uint32_t i = 0; i < n_in; ++i)
        {
          clap_note_port_info_t ninfo;
          if (ext_notes->get(tmpplug, i, true, &ninfo))
          {
            if (ninfo.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
            {
              result.has_midi_in = true;
              result.midi_in_name = ninfo.name;
              break;
            }
          }
        }
        uint32_t n_out = ext_notes->count(tmpplug, false);
        for (uint32_t i = 0; i < n_out; ++i)
        {
          clap_note_port_info_t ninfo;
          if (ext_notes->get(tmpplug, i, false, &ninfo))
          {
            if (ninfo.supported_dialects & CLAP_NOTE_DIALECT_MIDI)
            {
              result.has_midi_out = true;
              result.midi_out_name = ninfo.name;
              break;
            }
          }
        }
      }

#if (CLAP_WRAPPER_LOGLEVEL == 2)
      // logging out some details
      LOGDETAIL(fmt::format("the following configurations have been determined for plugin {}:",
                            tmpplug->desc->name));
      LOGDETAIL("--------------");
      for (auto &c : result.stemformats)
      {
        LOGDETAIL(fmt::format("  #{} Channels: {}/{}", c.name,
                              AAX_STEM_FORMAT_CHANNEL_COUNT(c.format_in),
                              AAX_STEM_FORMAT_CHANNEL_COUNT(c.format_out)));
      }
#endif
    }
    catch (std::exception &e)
    {
      LOGINFO("exception thrown during scan of plugin: {}", e.what());
    }
    catch (...)
    {
      LOGINFO("something got totally wrong");
    }
    tmpplug->destroy(tmpplug);
  }
  catch (std::exception &e)
  {
    LOGINFO("exception thrown: {}", e.what());
  }
  return result;
}
}  // namespace CLAPAAX