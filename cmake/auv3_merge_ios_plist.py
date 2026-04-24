#!/usr/bin/python3
"""
auv3_merge_ios_plist.py — POST_BUILD helper for target_add_auv3_wrapper on iOS.

Xcode's ProcessInfoPlistFile step stamps an iOS appex's Info.plist with a
handful of platform-validation keys that installd / pluginkit rely on at
install time:

    CFBundleSupportedPlatforms = ("iPhoneOS")
    DTPlatformName / DTPlatformVersion / DTPlatformBuild
    DTSDKName / DTSDKBuild
    DTCompiler / DTXcode / DTXcodeBuild
    BuildMachineOSBuild
    ...

The clap-wrapper AUv3 build then needs to inject AUv3-specific keys on top
of that (NSExtension with AudioComponents, CFBundlePackageType=XPC!, etc.).
On macOS the legacy behaviour was to overwrite Xcode's plist with the
template output wholesale, because macOS appex registration tolerates the
missing DT* / CFBundleSupportedPlatforms keys. On iOS it does not — the
extension fails to register and AVAudioUnitComponentManager reports 0
matches.

This script merges our AUv3-specific keys into Xcode's already-processed
plist instead of replacing it. Only the named keys are overlaid; every key
Xcode produced is preserved.

Usage:
    auv3_merge_ios_plist.py <xcode_processed_plist> <our_auv3_template_plist>
"""
import plistlib
import sys


# Keys we're allowed to overlay from the AUv3 template onto Xcode's plist.
# Everything else Xcode produces (DT*, CFBundleSupportedPlatforms, ...) must
# be kept untouched.
OVERLAY_KEYS = {
    "NSExtension",
    "CFBundlePackageType",
    "CFBundleDisplayName",
    "CFBundleDevelopmentRegion",
    "LSRequiresIPhoneOS",
}


def main(argv):
    if len(argv) != 3:
        print(f"usage: {argv[0]} <xcode_plist> <template_plist>", file=sys.stderr)
        return 2

    xcode_path, template_path = argv[1], argv[2]

    with open(xcode_path, "rb") as f:
        merged = plistlib.load(f)

    with open(template_path, "rb") as f:
        overlay = plistlib.load(f)

    for key in OVERLAY_KEYS:
        if key in overlay:
            merged[key] = overlay[key]

    with open(xcode_path, "wb") as f:
        plistlib.dump(merged, f)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
