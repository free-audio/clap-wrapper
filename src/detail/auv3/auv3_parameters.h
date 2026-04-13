#pragma once

/*
    AUv3 Parameter Bridge

    Copyright (c) 2024 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    Builds an AUParameterTree from CLAP parameters with proper grouping,
    value observation, and string conversion callbacks.
*/

#import <AudioToolbox/AudioToolbox.h>
#include <clap/clap.h>
#include <memory>

namespace Clap::AUv3
{

struct ParameterTreeResult
{
  AUParameterTree *tree;
  clap_id bypassParamId;  // CLAP_INVALID_ID if no bypass parameter found
};

// Build an AUParameterTree from the CLAP plugin's parameter extensions.
// The tree groups parameters by their module path (split on '/').
// The callbacks (implementorValueObserver, implementorValueProvider, etc.)
// are wired to the provided plugin and params extension.
// Also detects the CLAP_PARAM_IS_BYPASS parameter and returns its ID.
ParameterTreeResult createParameterTree(const clap_plugin_t *plugin, const clap_plugin_params_t *params);

}  // namespace Clap::AUv3
