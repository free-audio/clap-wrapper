#!/bin/sh
# ios_embed_appex.sh — POST_BUILD helper for target_add_auv3_standalone_ios_wrapper
#
# Copies an AUv3 .appex into the host app's PlugIns/ directory and re-signs
# the host. The appex keeps Xcode's signature (which matches the entitlements
# baked into its binary); only the host needs re-signing because embedding
# the appex changes the host's nested-code hash.
#
# Re-signing the host uses Xcode's selected code-sign identity (exported via
# EXPANDED_CODE_SIGN_IDENTITY during build script execution). On device
# builds that identity is the dev cert bound to the provisioning profile;
# for simulator builds the var is empty and we fall back to ad-hoc ("-"),
# which the simulator accepts.
#
# The caller MUST have set the AUv3 target's BUNDLE_IDENTIFIER so it's a
# prefix-child of the host's (iOS installd enforces this), because we no
# longer rewrite the bundle ID at embed time — doing so invalidates the
# appex's signed application-identifier entitlement and the AU fails to
# register at runtime (OSStatus -3000).
#
# Usage:
#   ios_embed_appex.sh <appex_src_dir> <appex_dst_dir> <host_bundle_dir>
set -eu

APPEX_SRC="$1"
APPEX_DST="$2"
HOST_DIR="$3"

IDENTITY="${EXPANDED_CODE_SIGN_IDENTITY:--}"

# A device build re-signed ad-hoc is rejected by installd at install time
# with no useful diagnostic — fail here with a clear message instead.
# (EXPANDED_CODE_SIGN_IDENTITY and PLATFORM_NAME are exported by Xcode to
# script phases; simulator builds legitimately fall back to ad-hoc.)
if [ "$IDENTITY" = "-" ] && [ "${PLATFORM_NAME:-}" = "iphoneos" ]; then
    echo "error: ios_embed_appex.sh: no code-sign identity for a device build" \
         "(EXPANDED_CODE_SIGN_IDENTITY is empty). Set DEVELOPMENT_TEAM /" \
         "a signing identity in Xcode, or build for the simulator." >&2
    exit 1
fi

# Clean + copy
rm -rf "$APPEX_DST"
mkdir -p "$(dirname "$APPEX_DST")"
cp -R "$APPEX_SRC" "$APPEX_DST"

# Re-sign the host (nested-code hash changed when we added the appex).
# --preserve-metadata=entitlements keeps the host's own entitlements that
# Xcode stamped into the binary at link time.
# --generate-entitlement-der emits DER-encoded entitlements required for iOS.
codesign --force --sign "$IDENTITY" --timestamp=none \
         --preserve-metadata=entitlements --generate-entitlement-der \
         "$HOST_DIR"
