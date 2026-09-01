#pragma once

#include "clap/private/macros.h"
#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

  static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_AUV2[] =
      "clap.plugin-factory-info-as-auv2.draft0";

  typedef struct clap_plugin_info_as_auv2
  {
    char au_type[5];  // the au_type. If empty (best choice) use the features[0] to aumu aufx aumi
    char au_subt[5];  // the subtype. If empty (worst choice) we try a bad 32 bit hash of the id
  } clap_plugin_info_as_auv2_t;

  typedef struct clap_plugin_factory_as_auv2
  {
    // optional values for the Steinberg::PFactoryInfo structure
    const char *manufacturer_code;  // your 'manu' field
    const char *manufacturer_name;  // your manufacturer display name

    // populate information about this particular auv2. If this method returns
    // false, the CLAP Plugin at the given index will not be exported into the
    // resulting AUv2
    bool(CLAP_ABI *get_auv2_info)(const clap_plugin_factory_as_auv2 *factory, uint32_t index,
                                  clap_plugin_info_as_auv2_t *info);
  } clap_plugin_factory_as_auv2_t;

  // An AudioUnit is identified by the triple (type, subtype, manufacturer), so
  // changing any of them -- an effect that grows a note port and has to become
  // an 'aumf' to be given one, a rebrand that moves the manufacturer code --
  // produces a component the host considers unrelated to the old one. Sessions
  // referencing the old identity do not find the new one and open without the
  // plugin.
  //
  // This factory lets a plugin keep answering to identities it used to have.
  // Each one is written into the bundle's Info.plist as an additional
  // AudioComponents entry, identical to the primary but for the identity
  // triple. Old sessions load; new ones get the current identity.
  //
  // The entries are ordinary and visible. kAudioComponentFlag_Unsearchable
  // looks like exactly the right flag here -- hide the retired identity from
  // browsing, keep it resolvable by full description -- and it does not work.
  // Logic restores a session against the registry its own AU scan builds, that
  // scan enumerates, and enumeration is what the flag excludes; measured
  // 01.09.2026, a hidden entry was reachable through AudioComponentFindNext and
  // the session that named it still would not open. Do not add the flag back.
  //
  // Listing costs nothing, which is why there is no option to hide. A host
  // filters a slot by component type, so an 'aufx' and an 'aumf' of the same
  // plugin never appear together -- an effect slot offers one, an instrument
  // slot the other. Two identities that *would* collide in one menu are two
  // identities the plugin should not be claiming.
  //
  // The wrapper reads this at *build* time, when it generates the Info.plist,
  // so it costs a plugin nothing at run time. A legacy identity that collides
  // with the primary one, or with another legacy identity, fails the build.
  //
  // Optional, and separate from clap_plugin_factory_as_auv2 on purpose: a
  // plugin adopting it does not restate what that one already answers, and the
  // wrapper's existing readers of that factory -- auv2 and auv3 -- are
  // untouched.
  static const CLAP_CONSTEXPR char CLAP_PLUGIN_FACTORY_INFO_AUV2_LEGACY[] =
      "clap.plugin-factory-info-as-auv2-legacy/0";

  typedef struct clap_plugin_auv2_legacy_identity
  {
    char au_type[5];  // the type this plugin used to have, e.g. "aufx"
    char au_subt[5];  // the subtype it used to have
    char au_manu[5];  // the manufacturer code it used to have
  } clap_plugin_auv2_legacy_identity_t;

  typedef struct clap_plugin_factory_auv2_legacy
  {
    // How many identities the CLAP plugin at `plugin_index` also answers to.
    // Zero, or an absent factory, means the plugin has never changed identity.
    uint32_t(CLAP_ABI *count)(const clap_plugin_factory_auv2_legacy *factory, uint32_t plugin_index);

    // Fills `identity` for legacy identity `n` of the plugin at `plugin_index`.
    // Returning false skips that entry.
    //
    // Every field must be four characters. An empty field is not "same as the
    // primary": an identity is a triple and a partial one cannot be resolved by
    // a host, so it is rejected rather than completed.
    bool(CLAP_ABI *get)(const clap_plugin_factory_auv2_legacy *factory, uint32_t plugin_index,
                        uint32_t n, clap_plugin_auv2_legacy_identity_t *identity);
  } clap_plugin_factory_auv2_legacy_t;

  // Parameter order matters in auv2 critically still in logic and garage band, and if you add
  // parameters after a release, you need to order them (alas) even if the ids aren't changed.
  // clap_plugin_auv2_param_ordering extension allows you to provide an ordering for your params
  // by mapping the get_param_info index (0..num-params) to a different ordering.
  //
  // The result of this will be used such that `auv2_index = ordering[clap_index]`. So in a five
  // parameter case, if you want clap index 0 to appear at auv2 position 3, and clap index 3 to appear
  // at auv2 position 0, you would return `3 1 2 0 4`
  //
  // The default behavior absent this extension is to sort by auv2 parameter id, which is clap param id.
  // So if you use a strategy where you go from not adopting this to adopting this when you add params,
  // make sure your old version param subset retains the param id ordering.
  //
  // The clap wrapper will check if your ordering is complete and valid, and if not, generate errors
  // to stdout and, in a debug build, fail an assertion. Running in auval while developing this
  // method with a debug build enabled is helpful.
  //
  // A reasonable implementation if your parameters have a 'version' increasing parameter could be
  //
  // static bool CLAP_ABI auv2_get_param_order(const clap_plugin_t *plugin, size_t *order,
  //   size_t param_count) noexcept
  // {
  //   auto *self = static_cast<SixSinesClap *>(plugin->plugin_data);
  //   auto &params = self->engine->patch.params; // your internal model!
  //   if (param_count != params.size())
  //     return false;
  //
  //   std::iota(order, order + param_count, (size_t)0);
  //   std::sort(order, order + param_count, [&params](size_t a, size_t b) {
  //       bool aVer = params[a]->meta.version;
  //       bool bVer = params[b]->meta.version;
  //       if (aVer != bVer)
  //       {
  //           return aVer < bVer;
  //       }
  //       return params[a]->meta.id < params[b]->meta.id;
  //   });
  //   return true;
  // }
  static const CLAP_CONSTEXPR char CLAP_PLUGIN_AUV2_PARAM_ORDERING[] =
      "clap.plugin-auv2-param-ordering/0";

  typedef struct clap_plugin_auv2_param_ordering
  {
    // given an empty input array order of size param_count, populate it with the index ordering.
    // return true if successful. if successful, the order array will contain each index 0...param_count-1
    // once and only once and the param at auv2_index will be the clap param at ordering[clap_index].
    bool(CLAP_ABI *get_param_order)(const clap_plugin_t *plugin, size_t *order, size_t param_count);
  } clap_plugin_auv2_param_ordering_t;

#ifdef __cplusplus
}
#endif
