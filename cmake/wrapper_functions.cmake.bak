# This is the meat of the clap wrapper cmake (which does quite a lot)
#
# The library is broken into several chunks
#
# shared_prologue defines things like platform, utility macros, and compile options interface.
# It also provides a function to eject the shared clap wrapper base targets
#
# base sdks provides mechanisms to resolve our code dependencies, CLAP, VST3, AudioUnitSDK,
# RtAudio and RtMidi providing a library like `base-sdk-vst3` target after you call the
# enablement function `guarantee_vst3sdk()`. Our pattern is to require a call to guarantee
# to create the target, so dependencies are lazy / as needed
#
# We then make sure that clap and clap shared are guaranteed and then load each of the wrap
# functions
#
# wrap_xyz.cmake provides a function target_add_xyz_wrapper. The arguments to these are relatively
# uniform (each takes a TARGET and OUTPUT_NAME) but are varied based on the build modes and requirements
# of each format XYZ.

include(cmake/shared_prologue.cmake)
include(cmake/base_sdks.cmake)

guarantee_clap()
guarantee_clap_wrapper_shared()


include(cmake/wrap_auv2.cmake)
include(cmake/wrap_vst3.cmake)
include(cmake/wrap_standalone.cmake)
include(cmake/wrap_clap.cmake)
include(cmake/wrap_wclap.cmake)

include(cmake/make_clapfirst.cmake)
