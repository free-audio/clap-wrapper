#include "process.h"

namespace Clap::AAX
{

void ProcessAdapter::setupProcessing(const clap_plugin_t* plugin, const clap_plugin_params_t* ext,
                                     uint32_t maxFrames) noexcept
{
  _plugin = plugin;
  _ext_params = ext;
  _processData.frames_count = maxFrames;
}

void ProcessAdapter::process(const AAXProcessContext& ctx) noexcept
{
  // Single bus handling arbitrary number of channels
  uint32_t channelCount = 2;  // assume stereo
  clap_audio_buffer_t bus{};
  bus.channel_count = channelCount;
  bus.constant_mask = 0;
  bus.data32 = const_cast<float**>(ctx.inputs);
  bus.data64 = nullptr;

  _processData.audio_inputs = &bus;
  _processData.audio_outputs = &bus;
  _processData.frames_count = ctx.frames;
  _processData.audio_inputs_count = 1;
  _processData.audio_outputs_count = 1;

  _plugin->process(_plugin, &_processData);
}
}  // namespace Clap::AAX
