/*
    wrapasauv3.mm

    Copyright (c) 2024 Timo Kaluza (defiantnerd)

    This file is part of the clap-wrappers project which is released under MIT License.
    See file LICENSE or go to https://github.com/free-audio/clap-wrapper for full license details.

    This file includes the generated entry points which create per-plugin factory subclasses.
    It serves as the compilation unit that ties together the generated code with the AUv3 wrapper.
*/

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wlanguage-extension-token"

#include "generated_auv3_entrypoints.hxx"

#pragma clang diagnostic pop

#include <dispatch/dispatch.h>

// App extension entry point.
// For in-process hosting, macOS loads the appex bundle directly and
// instantiates NSExtensionPrincipalClass — main() is never reached.
// For out-of-process hosting, macOS launches this executable and
// dispatch_main() keeps the process alive for XPC message handling.
int mainXXX(int, const char *[])
{
  // dispatch_main();
  return 0;
}
