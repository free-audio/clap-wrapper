#pragma once

/*
    a minimalistic OS layer
*/

#include <atomic>
#include <string>
#include <functional>

#include "log.h"
#include "fs.h"

namespace os
{
class State
{
  // the State class ensures that a specific function pair (like init/terminate) is only called in pairs.
  // the bool _state reflects if the _on lambda has already been called
  // on destruction, it makes sure that the _off lambda is definitely called.
  //
  // the construct should only be applied to when no state machine can cover this properly
  // or your own code depends on correct calling sequences of an external code base and should
  // help to implement defensive programming.

 public:
  State(std::function<void()> on, std::function<void()> off) : _on(on), _off(off)
  {
  }
  ~State()
  {
    off();
  }
  void on()
  {
    if (!_state)
    {
      _on();
    }
    _state = true;
  }
  void off()
  {
    if (_state)
    {
      _off();
    }
    _state = false;
  }

 private:
  bool _state = false;
  std::function<void()> _on, _off;
};

class IPlugObject
{
 public:
  virtual void onIdle() = 0;
  virtual ~IPlugObject()
  {
  }
};
void attach(IPlugObject* plugobject);
void detach(IPlugObject* plugobject);
uint64_t getTickInMS();

// Used for clap_plugin_entry.init(). Path to DSO (Linux, Windows), or the bundle (macOS).
fs::path getModulePath();
std::string getParentFolderName();
std::string getBinaryName();
}  // namespace os
