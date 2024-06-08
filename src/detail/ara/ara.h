#pragma once

#include <clap/clap.h>

// ARA support
//

//! Factory ID for retrieving the clap_ara_factory_t extension from clap_plugin_entry_t.get_factory()
static CLAP_CONSTEXPR const char CLAP_EXT_ARA_FACTORY[] = "com.celemony.ara.factory.draft/1";

//! Extension ID for retrieving the clap_ara_plugin_extension_t from clap_plugin_t.get_extension()
static CLAP_CONSTEXPR const char CLAP_EXT_ARA_PLUGIN_EXTENSION[] =
    "com.celemony.ara.plugin_extension.draft/1";

//! Add this feature if your plugin requires ARA to operate (will not work as normal insert plug-in).
//! This allows non-ARA CLAP hosts to suppress the plug-in since it cannot be used there.
#define CLAP_PLUGIN_FEATURE_ONLY_ARA "only-ara"

// VST3 class category name
#define kARAMainFactoryClass "ARA Main Factory Class"

#ifdef __cplusplus
extern "C"
{
#endif

  // substitute types so we don't need to include the ARA SDK in full
  typedef void* ARAFactoryPtr;
  typedef void* ARAPlugInExtensionInstancePtr;
  typedef void* ARADocumentControllerRef;
  typedef int32_t ARAPlugInInstanceRoleFlags;

  typedef struct clap_ara_factory
  {
    //! Get the number of ARA factories (i.e. ARA-capable plug.-ins) available.
    //! Note that the regular clap_plugin_factory can contain more plug-ins if these do not support
    //! ARA - make no assumption about items returned here being related to the items returned there
    //! in terms of count or order.
    uint32_t(CLAP_ABI* get_factory_count)(const struct clap_ara_factory* factory);

    //! Get the ARA factory for the plug-in at the given index.
    //! The returned pointer must remain valid until clap_plugin_entry_t.deinit() is called.
    //! The returned ARAFactory must be equal to the ARAFactory returned from instances of the
    //! associated CLAP plug-in through their clap_ara_plugin_extension_t.get_factory().
    ARAFactoryPtr(CLAP_ABI* get_ara_factory)(const struct clap_ara_factory* factory, uint32_t index);

    //! Get the ID of the CLAP plug-in associated with the ARA factory for the given index.
    //! The plug-in must be in the same binary.
    //! The returned pointer must remain valid until clap_plugin_entry_t.deinit is called.
    const char*(CLAP_ABI* get_plugin_id)(const struct clap_ara_factory* factory, uint32_t index);
  } clap_ara_factory_t;

  typedef struct clap_ara_plugin_extension
  {
    //! Access the ARAFactory associated with this plug-in.
    ARAFactoryPtr(CLAP_ABI* get_factory)(const clap_plugin_t* plugin);

    //! Bind the CLAP instance to an ARA document controller, switching it from "normal" operation
    //! to ARA mode with the assigned roles, and exposing the ARA plug-in extension.
    //! \p knownRoles encodes all roles that the host considered in its implementation and will explicitly
    //! assign to some plug-in instance(s), while \p assignedRoles describes the roles that this specific
    //! instance will fulfill.
    //! This may be called only once during the lifetime of the CLAP plug-in, before the first call
    //! to clap_plugin_t.activate() or clap_host_state_t.load() or other processing related extensions
    //! or the creation of the GUI.
    //! The ARA document controller must remain valid as long as the plug-in is in use - rendering,
    //! showing its UI, etc. However, when tearing down the plug-in, the actual order for deleting
    //! the clap_plugin_t instance and for deleting ARA document controller is undefined.
    //! Plug-ins must handle both potential destruction orders to allow for a simpler reference
    //! counting implementation on the host side.
    ARAPlugInExtensionInstancePtr(CLAP_ABI* bind_to_document_controller)(
        const clap_plugin_t* plugin, ARADocumentControllerRef documentControllerRef,
        ARAPlugInInstanceRoleFlags knownRoles, ARAPlugInInstanceRoleFlags assignedRoles);
  } clap_ara_plugin_extension_t;

#ifdef __cplusplus
}
#endif
