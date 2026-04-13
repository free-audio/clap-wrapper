#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"

#include "auv3_parameters.h"

#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <string>
#include <vector>
#include <map>

namespace Clap::AUv3
{

// Split a module path like "Filter/Cutoff" into ["Filter", "Cutoff"]
static std::vector<std::string> splitModulePath(const char *module)
{
  std::vector<std::string> parts;
  if (!module || !module[0]) return parts;

  std::string path(module);
  size_t pos = 0;
  while ((pos = path.find('/')) != std::string::npos)
  {
    auto part = path.substr(0, pos);
    if (!part.empty()) parts.push_back(part);
    path = path.substr(pos + 1);
  }
  if (!path.empty()) parts.push_back(path);
  return parts;
}

// Recursive tree node for building parameter groups
struct GroupNode
{
  std::string name;
  std::map<std::string, std::unique_ptr<GroupNode>> children;
  NSMutableArray<AUParameter *> *parameters = nil;

  GroupNode() : parameters([NSMutableArray new])
  {
  }

  AUParameterGroup *toGroup()
  {
    NSMutableArray *groupChildren = [NSMutableArray new];

    // Add sub-groups first
    for (auto &[childName, child] : children)
    {
      [groupChildren addObject:child->toGroup()];
    }
    // Then parameters
    for (AUParameter *p in parameters)
    {
      [groupChildren addObject:p];
    }

    return [AUParameterTree createGroupWithIdentifier:[NSString stringWithUTF8String:name.c_str()]
                                                 name:[NSString stringWithUTF8String:name.c_str()]
                                             children:groupChildren];
  }
};

ParameterTreeResult createParameterTree(const clap_plugin_t *plugin, const clap_plugin_params_t *params)
{
  if (!params) return
    {
      [AUParameterTree createTreeWithChildren:@[]], CLAP_INVALID_ID
    };

  uint32_t numParams = params->count(plugin);
  if (numParams == 0) return
    {
      [AUParameterTree createTreeWithChildren:@[]], CLAP_INVALID_ID
    };

  clap_id bypassParamId = CLAP_INVALID_ID;

  // Root group node for building hierarchy
  GroupNode root;
  root.name = "Root";

  // Top-level parameters (no module path)
  NSMutableArray<AUParameterNode *> *topLevelChildren = [NSMutableArray new];

  for (uint32_t i = 0; i < numParams; ++i)
  {
    clap_param_info_t info;
    if (!params->get_info(plugin, i, &info)) continue;

    // Determine AU parameter unit
    AudioUnitParameterUnit unit = kAudioUnitParameterUnit_Generic;
    AudioUnitParameterOptions flags =
        kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsHighResolution |
        kAudioUnitParameterFlag_HasCFNameString | kAudioUnitParameterFlag_CFNameRelease;

    bool isStepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
    bool isHidden = (info.flags & CLAP_PARAM_IS_HIDDEN) != 0;
    bool isReadonly = (info.flags & CLAP_PARAM_IS_READONLY) != 0;
    bool isAutomatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
    bool isBypass = (info.flags & CLAP_PARAM_IS_BYPASS) != 0;

    if (isBypass) bypassParamId = info.id;

    if (isHidden) continue;  // skip hidden parameters

    if (!isReadonly && isAutomatable)
    {
      flags |= kAudioUnitParameterFlag_IsWritable;
    }

    if (isStepped)
    {
      if (info.min_value == 0.0 && info.max_value == 1.0)
      {
        unit = kAudioUnitParameterUnit_Boolean;
      }
      else
      {
        unit = kAudioUnitParameterUnit_Indexed;
      }
    }

    NSString *identifier = [NSString stringWithFormat:@"clap_%llu", (unsigned long long)info.id];
    NSString *displayName = [NSString stringWithUTF8String:info.name];

    AUParameter *param = [AUParameterTree createParameterWithIdentifier:identifier
                                                                   name:displayName
                                                                address:(AUParameterAddress)info.id
                                                                    min:(AUValue)info.min_value
                                                                    max:(AUValue)info.max_value
                                                                   unit:unit
                                                               unitName:nil
                                                                  flags:flags
                                                           valueStrings:nil
                                                    dependentParameters:nil];
    param.value = (AUValue)info.default_value;

    // Place in group hierarchy based on module path
    auto moduleParts = splitModulePath(info.module);
    if (moduleParts.empty())
    {
      [topLevelChildren addObject:param];
    }
    else
    {
      // Navigate/create the group hierarchy
      GroupNode *current = &root;
      for (const auto &part : moduleParts)
      {
        auto it = current->children.find(part);
        if (it == current->children.end())
        {
          auto node = std::make_unique<GroupNode>();
          node->name = part;
          current->children[part] = std::move(node);
          current = current->children[part].get();
        }
        else
        {
          current = it->second.get();
        }
      }
      [current->parameters addObject:param];
    }
  }

  // Build the final tree: top-level groups + ungrouped params
  for (auto &[childName, child] : root.children)
  {
    [topLevelChildren addObject:child->toGroup()];
  }

  AUParameterTree *tree = [AUParameterTree createTreeWithChildren:topLevelChildren];

  // Wire up the callbacks using the plugin/params pointers.
  // These blocks capture the raw pointers - they must remain valid for the lifetime of the tree.
  const clap_plugin_t *capturedPlugin = plugin;
  const clap_plugin_params_t *capturedParams = params;

  tree.implementorValueProvider = ^AUValue(AUParameter *param) {
    double value = 0;
    capturedParams->get_value(capturedPlugin, (clap_id)param.address, &value);
    return (AUValue)value;
  };

  tree.implementorStringFromValueCallback = ^NSString *(AUParameter *param, const AUValue *value) {
    char buf[256];
    AUValue v = value ? *value : param.value;
    if (capturedParams->value_to_text(capturedPlugin, (clap_id)param.address, (double)v, buf,
                                      sizeof(buf)))
    {
      return [NSString stringWithUTF8String:buf];
    }
    return [NSString stringWithFormat:@"%.3f", v];
  };

  tree.implementorValueFromStringCallback = ^AUValue(AUParameter *param, NSString *string) {
    double value = 0;
    if (capturedParams->text_to_value(capturedPlugin, (clap_id)param.address, [string UTF8String],
                                      &value))
    {
      return (AUValue)value;
    }
    return (AUValue)[string doubleValue];
  };

  return {tree, bypassParamId};
}

}  // namespace Clap::AUv3

#pragma clang diagnostic pop
