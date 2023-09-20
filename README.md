# The clap-wrapper Project

The `clap-wrapper` project allows you to wrap your CLAP plugin
into other plugin or standalone executable formats, letting your
plugin run in hosts which don't support the CLAP format and 
allowing you to distribute standalone executable versions of your 
CLAP.

Our [wiki](wiki) is the root of our developer documentation.

Currently the `clap-wrapper` supports projecting a CLAP into

- VST3
- A Simple Standalone
- A scanning-but-non-functioning AUv2

The `clap-wrapper` also provides a variety of deployment,
linkage and build time options to CLAP developers.

## The Core Idea

The core idea of `clap-wrapper` is as follows

1. The `clap-wrapper` is an implementation of a clap host which
will load your CLAP using a variety of techniques.
2. The `clap-wrapper` provides an implementation of a plugin format
or standalone executable which implements the format to the host
or OS, but uses that host to load your clap.

And with that, voila, your CLAP can appear in a VST3 or AUv2
host or can appear to be a self contained standalone executable.

## Getting Started

Our [wiki](wiki) is the root of our developer documentation.
Since building a `clap-wrapper` involves several choices we
strongly suggest you consult it first.

The simplest form of a clap wraper, though, is a VST3
which dynamically loads a CLAP of the same name using your
copy of the VST3 and CLAP SDK. To build this, you can use the
simple CMake command

```bash
git clone https://github.com/free-audio/clap-wrapper.git
mkdir build
cmake -B build \ 
      -DCLAP_SDK_ROOT={path to clap sdk} \
      -DVST3_SDK_ROOT={path to vst3 sdk} \
      -DCLAP_WRAPPER_OUTPUT_NAME="The Name of your CLAP"
```

## Licensing

The `clap-wrapper` project is released under the MIT license.
Please note that using the `clap-wrapper` project to wrap
a VST3 requires either (1) you use the VST3 SDK under the GPL3
license, and as such are open source or (2) you agree to the
VST3 license to generate the wrapper.