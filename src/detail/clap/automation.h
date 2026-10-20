#pragma once

#include <clap/clap.h>

namespace Clap
{
class IAutomation
{
 public:
  virtual void onBeginEdit(clap_id id) = 0;
  virtual void onPerformEdit(const clap_event_param_value_t *value) = 0;
  virtual void onEndEdit(clap_id id) = 0;

  // A host moved the preset-selector parameter. Defaulted because it only
  // means anything to a wrapper that publishes a preset list, and because
  // this arrives on the audio thread: the implementation must marshal, not
  // load. \see Clap::PresetIndex
  virtual void onRequestPresetLoad(size_t /*presetIndex*/)
  {
  }
  virtual ~IAutomation()
  {
  }
};
}  // namespace Clap
