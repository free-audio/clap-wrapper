
#if WIN
#include <Windows.h>
#include <stdio.h>

#include "wrapasvst3.h"
#include "clap_proxy.h"
#include "pluginterfaces/base/ftypes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/common/memorystream.h"

HINSTANCE ghInst = nullptr;
void* moduleHandle = nullptr;

int main(void*)
{
    // loaded
    
  struct vst3plug
  {

    };
//    IPtr<Vst::IComponent> p;
//     p = owned(ClapAsVst3::createInstance(nullptr));
    auto ik = ClapAsVst3::createInstance(nullptr);
    void* v;
    Steinberg::FUnknownPtr<Steinberg::Vst::IComponent> component;
    Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> audioprocessor;
    auto r = ik->queryInterface(Steinberg::Vst::IComponent_iid, &v);
    component = static_cast<Steinberg::Vst::IComponent*>(v);
    r = ik->queryInterface(Steinberg::Vst::IAudioProcessor_iid, &v);
    audioprocessor = static_cast<Steinberg::Vst::IAudioProcessor*>(v);
    if (r == kResultOk)
    {
      auto k = component;
      auto ka = audioprocessor;
      //auto k = new ClapAsVst3(n, 0, nullptr);
      k->initialize(nullptr);
      Steinberg::Vst::ProcessSetup s;
      s.maxSamplesPerBlock = 512;
      s.sampleRate = 44100;
      s.processMode = Vst::ProcessModes::kRealtime;

      Steinberg::MemoryStream memstream;

      k->getState(&memstream);
      memstream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
      k->setState(&memstream);

      ka->setupProcessing(s);

      k->setActive(true);

      ka->setProcessing(true);

      // render

      ka->setProcessing(false);

      k->setActive(false);

      // ka->removeAllBusses();

      k->terminate();

      ka->release();
      k->release();
      ik->release();
    }


  

  // auto k = ClapAsVst3::createInstance(nullptr);
  
  return 0;

}
#else
#include <iostream>
int main(int argc, char **argv)
{
   std::cout << "MAIN does not yet work on this platform" << std::endl;
}
#endif