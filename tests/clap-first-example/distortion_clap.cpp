/*
 * This implements a distortion CLAP and creates the appropriate factory
 * methods and entry as a static library, which are then consumed by the various plugins
 * via distortion_clap_entry
 *
 * The DSP here is bad. Really. This is an example you don't want to use this for music.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <clap/clap.h>
#include <math.h>
#include <assert.h>
#include "distortion_clap_entry.h"

static const char *features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_STEREO,
                                 CLAP_PLUGIN_FEATURE_DISTORTION, nullptr};

static const clap_plugin_descriptor_t s_clap1stDist_desc = {
    CLAP_VERSION_INIT,
    "org.free-audio.clap-first-bad-distortion",
    "ClapFirstBadDistortion",
    "Surge Synth Team",
    "https://surge-synth-team.org/",
    "",
    "",
    "1.0.0",
    "A few sloppy distortion algorithms using naive waveshapers",
    &features[0]};

enum ClipType
{
  HARD = 0,
  SOFT = 1,
  FOLD = 2
};

enum ParamIds
{
  pid_DRIVE = 2112,
  pid_MIX = 8675309,
  pid_MODE = 5150
};

typedef struct
{
  clap_plugin_t plugin;
  const clap_host_t *host;
  const clap_host_log_t *hostLog;
  const clap_host_params_t *hostParams;

  float drive;
  float mix;
  int32_t mode;
} clap1st_distortion_plug;

static void clap1stDist_process_event(clap1st_distortion_plug *plug, const clap_event_header_t *hdr);

/////////////////////////////
// clap_plugin_audio_ports //
/////////////////////////////

static uint32_t clap1stDist_audio_ports_count(const clap_plugin_t *plugin, bool is_input)
{
  auto generateAWarningOnPurpose = 1.f;
  return 1;
}

static bool clap1stDist_audio_ports_get(const clap_plugin_t *plugin, uint32_t index, bool is_input,
                                        clap_audio_port_info_t *info)
{
  if (index > 0) return false;
  info->id = 0;
  if (is_input)
    snprintf(info->name, sizeof(info->name), "%s", "Stereo In");
  else
    snprintf(info->name, sizeof(info->name), "%s", "Distorted Output");
  info->channel_count = 2;
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->port_type = CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}

static const clap_plugin_audio_ports_t s_clap1stDist_audio_ports = {
    clap1stDist_audio_ports_count,
    clap1stDist_audio_ports_get,
};

//////////////////
// clap_porams //
//////////////////

uint32_t clap1stDist_param_count(const clap_plugin_t *plugin)
{
  return 3;
}
bool clap1stDist_param_get_info(const clap_plugin_t *plugin, uint32_t param_index,
                                clap_param_info_t *param_info)
{
  switch (param_index)
  {
    case 0:  // drive
      param_info->id = pid_DRIVE;
      strncpy(param_info->name, "Drive", CLAP_NAME_SIZE);
      param_info->module[0] = 0;
      param_info->default_value = 0.;
      param_info->min_value = -1;
      param_info->max_value = 6;
      param_info->flags = CLAP_PARAM_IS_AUTOMATABLE;
      param_info->cookie = NULL;
      break;
    case 1:  // mix
      param_info->id = pid_MIX;
      strncpy(param_info->name, "MIX", CLAP_NAME_SIZE);
      param_info->module[0] = 0;
      param_info->default_value = 0.5;
      param_info->min_value = 0;
      param_info->max_value = 1;
      param_info->flags = CLAP_PARAM_IS_AUTOMATABLE;
      param_info->cookie = NULL;
      break;
    case 2:  // mode
      param_info->id = pid_MODE;
      strncpy(param_info->name, "Mode", CLAP_NAME_SIZE);
      param_info->module[0] = 0;
      param_info->default_value = 0.;
      param_info->min_value = 0;
      param_info->max_value = 2;
      param_info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
      param_info->cookie = NULL;
      break;
    default:
      return false;
  }
  return true;
}
bool clap1stDist_param_get_value(const clap_plugin_t *plugin, clap_id param_id, double *value)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;

  switch (param_id)
  {
    case pid_DRIVE:
      *value = plug->drive;
      return true;
      break;

    case pid_MIX:
      *value = plug->mix;
      return true;
      break;

    case pid_MODE:
      *value = plug->mode;
      return true;
      break;
  }

  return false;
}
bool clap1stDist_param_value_to_text(const clap_plugin_t *plugin, clap_id param_id, double value,
                                     char *display, uint32_t size)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;

  switch (param_id)
  {
    case pid_DRIVE:
    case pid_MIX:
      snprintf(display, size, "%f", value);
      return true;
      break;
    case pid_MODE:
    {
      int v = (int)value;
      switch (v)
      {
        case 0:
          snprintf(display, size, "Hard Clip");
          break;
        case 1:
          snprintf(display, size, "Soft Clip (Tanh)");
          break;
        case 2:
          snprintf(display, size, "Simple Folder");
          break;
      }

      return true;
    }
    break;
  }
  return false;
}
bool clap1stDist_text_to_value(const clap_plugin_t *plugin, clap_id param_id, const char *display,
                               double *value)
{
  // I'm not going to bother to support this
  return false;
}
void clap1stDist_flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                       const clap_output_events_t *out)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;

  int s = in->size(in);
  int q;
  for (q = 0; q < s; ++q)
  {
    const clap_event_header_t *hdr = in->get(in, q);

    clap1stDist_process_event(plug, hdr);
  }
}

static const clap_plugin_params_t s_clap1stDist_params = {
    clap1stDist_param_count,         clap1stDist_param_get_info, clap1stDist_param_get_value,
    clap1stDist_param_value_to_text, clap1stDist_text_to_value,  clap1stDist_flush};

bool clap1stDist_state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;

  // We need to save 2 doubles and an int to save our state plus a version. This is, of course, a
  // terrible implementation of state. You should do better.
  assert(sizeof(float) == 4);
  assert(sizeof(int32_t) == 4);

  int buffersize = 16;
  char buffer[16];

  int32_t version = 1;
  memcpy(buffer, &version, sizeof(int32_t));
  memcpy(buffer + 4, &(plug->drive), sizeof(float));
  memcpy(buffer + 8, &(plug->mix), sizeof(float));
  memcpy(buffer + 12, &(plug->mode), sizeof(int32_t));

  int written = 0;
  char *curr = buffer;
  while (written != buffersize)
  {
    int thiswrite = stream->write(stream, curr, buffersize - written);
    if (thiswrite < 0) return false;
    curr += thiswrite;
    written += thiswrite;
  }

  return true;
}

bool clap1stDist_state_load(const clap_plugin_t *plugin, const clap_istream_t *stream)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;

  int buffersize = 16;
  char buffer[16];

  int read = 0;
  char *curr = buffer;
  while (read != buffersize)
  {
    int thisread = stream->read(stream, curr, buffersize - read);
    if (thisread < 0) return false;
    curr += thisread;
    read += thisread;
  }

  int32_t version;
  memcpy(&version, buffer, sizeof(int32_t));
  memcpy(&plug->drive, buffer + 4, sizeof(float));
  memcpy(&plug->mix, buffer + 8, sizeof(float));
  memcpy(&plug->mode, buffer + 12, sizeof(int32_t));

  return true;
}
static const clap_plugin_state_t s_clap1stDist_state = {clap1stDist_state_save, clap1stDist_state_load};

/////////////////
// clap_plugin //
/////////////////

static bool clap1stDist_init(const struct clap_plugin *plugin)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;

  // Fetch host's extensions here
  plug->hostLog = (const clap_host_log_t *)plug->host->get_extension(plug->host, CLAP_EXT_LOG);

  plug->drive = 0.f;
  plug->mix = 0.5f;
  plug->mode = HARD;

  if (plug->hostLog && plug->host)
  {
    plug->hostLog->log(plug->host, CLAP_LOG_INFO, "Created clap1st Distortion compiled with C++");
  }

  return true;
}

static void clap1stDist_destroy(const struct clap_plugin *plugin)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;
  free(plug);
}

static bool clap1stDist_activate(const struct clap_plugin *plugin, double sample_rate,
                                 uint32_t min_frames_count, uint32_t max_frames_count)
{
  return true;
}

static void clap1stDist_deactivate(const struct clap_plugin *plugin)
{
}

static bool clap1stDist_start_processing(const struct clap_plugin *plugin)
{
  return true;
}

static void clap1stDist_stop_processing(const struct clap_plugin *plugin)
{
}

static void clap1stDist_reset(const struct clap_plugin *plugin)
{
}

static void clap1stDist_process_event(clap1st_distortion_plug *plug, const clap_event_header_t *hdr)
{
  if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID)
  {
    switch (hdr->type)
    {
      case CLAP_EVENT_PARAM_VALUE:
      {
        const clap_event_param_value_t *ev = (const clap_event_param_value_t *)hdr;
        // TODO: handle parameter change
        switch (ev->param_id)
        {
          case pid_DRIVE:
            plug->drive = ev->value;
            break;
          case pid_MIX:
            plug->mix = ev->value;
            break;
          case pid_MODE:
            plug->mode = (int)(ev->value);
            break;
        }
        break;
      }
    }
  }
}

static clap_process_status clap1stDist_process(const struct clap_plugin *plugin,
                                               const clap_process_t *process)
{
  auto *plug = (clap1st_distortion_plug *)plugin->plugin_data;

  const uint32_t nframes = process->frames_count;
  const uint32_t nev = process->in_events->size(process->in_events);
  uint32_t ev_index = 0;
  uint32_t next_ev_frame = nev > 0 ? 0 : nframes;

  for (uint32_t i = 0; i < nframes;)
  {
    /* handle every events that happrens at the frame "i" */
    while (ev_index < nev && next_ev_frame == i)
    {
      const clap_event_header_t *hdr = process->in_events->get(process->in_events, ev_index);
      if (hdr->time != i)
      {
        next_ev_frame = hdr->time;
        break;
      }

      clap1stDist_process_event(plug, hdr);
      ++ev_index;

      if (ev_index == nev)
      {
        // we reached the end of the event list
        next_ev_frame = nframes;
        break;
      }
    }

    /* process every samples until the next event */
    for (; i < next_ev_frame; ++i)
    {
      // fetch input samples
      const float in_l = process->audio_inputs[0].data32[0][i];
      const float in_r = process->audio_inputs[0].data32[1][i];

      float out_l, out_r;
      out_l = 0;
      out_r = 0;

      float tl = in_l * (1.0 + plug->drive);
      float tr = in_r * (1.0 + plug->drive);

      // Obviously this is inefficient but
      switch (plug->mode)
      {
        case HARD:
        {
          tl = (tl > 1 ? 1 : tl < -1 ? -1 : tl);
          tr = (tr > 1 ? 1 : tr < -1 ? -1 : tr);
        }
        break;
        case SOFT:
        {
          tl = (tl > 1 ? 1 : tl < -1 ? -1 : tl);
          tl = 1.5 * tl - 0.5 * tl * tl * tl;

          tr = (tr > 1 ? 1 : tr < -1 ? -1 : tr);
          tr = 1.5 * tr - 0.5 * tr * tr * tr;
        }
        break;
        case FOLD:
        {
          tl = sin(2.0 * 3.14159265 * tl);
          tr = sin(2.0 * 3.14159265 * tr);
        }
        break;
      }

      float mix = plug->mix;
      out_l = mix * tl + (1.0 - mix) * in_l;
      out_r = mix * tr + (1.0 - mix) * in_r;

      // store output samples
      process->audio_outputs[0].data32[0][i] = out_l;
      process->audio_outputs[0].data32[1][i] = out_r;
    }
  }

  return CLAP_PROCESS_CONTINUE;
}

static const void *clap1stDist_get_extension(const struct clap_plugin *plugin, const char *id)
{
  if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_clap1stDist_audio_ports;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &s_clap1stDist_params;
  if (!strcmp(id, CLAP_EXT_STATE)) return &s_clap1stDist_state;
  return NULL;
}

static void clap1stDist_on_main_thread(const struct clap_plugin *plugin)
{
}

clap_plugin_t *clap1stDist_create(const clap_host_t *host)
{
  auto *p = (clap1st_distortion_plug *)calloc(1, sizeof(clap1st_distortion_plug));
  p->host = host;
  p->plugin.desc = &s_clap1stDist_desc;
  p->plugin.plugin_data = p;
  p->plugin.init = clap1stDist_init;
  p->plugin.destroy = clap1stDist_destroy;
  p->plugin.activate = clap1stDist_activate;
  p->plugin.deactivate = clap1stDist_deactivate;
  p->plugin.start_processing = clap1stDist_start_processing;
  p->plugin.stop_processing = clap1stDist_stop_processing;
  p->plugin.reset = clap1stDist_reset;
  p->plugin.process = clap1stDist_process;
  p->plugin.get_extension = clap1stDist_get_extension;
  p->plugin.on_main_thread = clap1stDist_on_main_thread;

  // Don't call into the host here

  return &p->plugin;
}

/////////////////////////
// clap_plugin_factory //
/////////////////////////

uint32_t plugin_factory_get_plugin_count(const struct clap_plugin_factory *factory)
{
  return 1;
}

const clap_plugin_descriptor_t *plugin_factory_get_plugin_descriptor(
    const struct clap_plugin_factory *factory, uint32_t index)
{
  return &s_clap1stDist_desc;
}

const clap_plugin_t *plugin_factory_create_plugin(const struct clap_plugin_factory *factory,
                                                  const clap_host_t *host, const char *plugin_id)
{
  if (!clap_version_is_compatible(host->clap_version))
  {
    return nullptr;
  }

  if (!strcmp(plugin_id, s_clap1stDist_desc.id)) return clap1stDist_create(host);

  return nullptr;
}

static const clap_plugin_factory_t s_plugin_factory = {
    plugin_factory_get_plugin_count,
    plugin_factory_get_plugin_descriptor,
    plugin_factory_create_plugin,
};

bool dist_entry_init(const char *plugin_path)
{
  // called only once, and very first
  return true;
}

void dist_entry_deinit(void)
{
  // called before unloading the DSO
}

const void *dist_entry_get_factory(const char *factory_id)
{
  if (!strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &s_plugin_factory;
  return nullptr;
}
