#pragma once

#include <clap/clap.h>
#include <clap_proxy.h>
#include <string>

namespace Clap::Standalone
{
std::shared_ptr<Clap::Plugin> mainCreatePlugin(const clap_plugin_entry *entry, const std::string &clapId,
                                               uint32_t clapIndex, int argc, char **argv);
void mainStartAudio();

std::shared_ptr<Clap::Plugin> getMainPlugin();
int mainWait();
int mainFinish();
}  // namespace Clap::Standalone