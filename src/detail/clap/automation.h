#pragma once

#include <clap/clap.h>

namespace Clap
{
  class IAutomation
  {
   public:
    virtual void onBeginEdit(clap_id id) = 0;
    virtual void onPerformEdit(const clap_event_param_value_t* value) = 0;
    virtual void onEndEdit(clap_id id) = 0;
    virtual ~IAutomation()
    {
    }
  };
}  // namespace Clap