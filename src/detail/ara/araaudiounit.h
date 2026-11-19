#pragma once

#include <cstddef>
#include <cstdint>
#include "ara.h"

// ARA support for the AUv2 wrapper
//
// the identifiers and structs are a minimal copy from the ARA SDK (file ARA_API/ARAAudioUnit.h from https://github.com/Celemony/ARA_API).
// ARA is Copyright (c) 2012-2025, Celemony Software GmbH, All Rights Reserved.
// Developed in cooperation with PreSonus Software Ltd.
//
// The ARA SDK is available under Apache-2.0 license here:
//
//    https://github.com/Celemony/ARA_SDK
//
// ARA types that are not needed here (ARAFactory, ARAPlugInExtensionInstance) are replaced by opaque
// pointers, see ara.h. The wrapper does not require the ARA SDK.
//
// do not use this file in your clap plugin, use the ARA SDK!
//

//! Audio Component tag that marks an Audio Unit as ARA capable in the AudioComponents dict of its Info.plist
#define kARAAudioComponentTag "ARA"

//! Property data for kAudioUnitProperty_ARAFactory.
struct ARAAudioUnitFactory
{
  //! Input/output: token to identify the property is actually used for ARA.
  uint32_t inOutMagicNumber;  // OSType
  //! Output: pointer to the factory associated with the Audio Unit.
  ARAFactoryPtr outFactory;
};

//! Property data for kAudioUnitProperty_ARAPlugInExtensionBindingWithRoles.
struct ARAAudioUnitPlugInExtensionBinding
{
  //! Input/output: token to identify the property is actually used for ARA.
  uint32_t inOutMagicNumber;  // OSType
  //! Input: document controller of the model graph that the Audio Unit shall be bound to.
  ARADocumentControllerRef inDocumentControllerRef;
  //! Output: the entry point for the plug-in.
  ARAPlugInExtensionInstancePtr outPlugInExtension;
  //! Input: all roles that the host considered in its implementation and will explicitly
  //! assign to some plug-in instance(s).
  ARAPlugInInstanceRoleFlags knownRoles;
  //! Input: roles assigned to this specific plug-in instance.
  ARAPlugInInstanceRoleFlags assignedRoles;
};

enum : uint32_t
{
  //! This value must be placed in the magicNumber fields of the ARA properties.
  kARAAudioUnitMagic = 'Ara!',

  //! kAudioUnitScope_Global, read-only, property data is ARAAudioUnitFactory:
  //! query the ARA factory associated with the given plug-in.
  kAudioUnitProperty_ARAFactory = 'AraF',

  //! kAudioUnitScope_Global, read-only, property data is ARAAudioUnitPlugInExtensionBinding:
  //! bind the Audio Unit instance to an ARA document controller with the assigned roles, and
  //! expose the ARA plug-in extension.
  //! This may be done only once during the lifetime of the Audio Unit, before initializing it
  //! or setting its state.
  kAudioUnitProperty_ARAPlugInExtensionBindingWithRoles = 'AraE',

  //! Deprecated ARA 1 binding, property data is ARAAudioUnitPlugInExtensionBinding without the
  //! knownRoles and assignedRoles fields. ARA 1 hosts use every instance as playback renderer,
  //! edit renderer and editor view.
  kAudioUnitProperty_ARAPlugInExtensionBinding = 'AraB'
};
