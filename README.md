# clap-wrapper
Wrappers for other plugin formats to the CLAP audio plugin format (see https://github.com/free-audio/clap)

## General Idea

This project does provide:
- a standalone wrapper that dynamically loads an installed CLAP
- a CMAKE library to make use of wrapper specific extensions

Future releases will provide:
- a library that is statically linked to a CLAP plugin.

It exposes the entrypoints for different Audio Plugin standards and their interfaces and maps them quite immediately to the CLAP in the same binary (this might be dynamic later on). The code locates the CLAP entrypoint and uses it from a host perspective and translates all information and events to a different Audio Standard.

The CLAP community calls this a PALCer or a CLAP-as-X wrapper.

## Status

Currently this works on Windows, macOS and Linux(*) and it will always try to load a .clap with the same name as the .vst3 from the CLAP folders. It does this by taking also the parent folder into account, so a vendor name can be matched and finally searches through the first level of subdirectories in the CLAP folders.

If not found, it returns a nullptr and the host will not load it.

Things currently missing:

- CMake function to link to clap project

## How to build

```bash
git clone https://github.com/free-audio/clap-wrapper.git
mkdir build
cmake -B build -DCLAP_SDK_ROOT={path to clap sdk} -DVST3_SDK_ROOT={path to vst3 sdk}
```

You can also determine the output name of the resulting VST3 plugin by using the `CLAP_WRAPPER_OUTPUT_NAME` CMake variable:

Build it for your clap, assuming it is named "fruit":

```bash
git clone https://github.com/free-audio/clap-wrapper.git
mkdir build
cmake -B build -DCLAP_SDK_ROOT={path to clap sdk} -DVST3_SDK_ROOT={path to vst3 sdk} -DCLAP_WRAPPER_OUTPUT_NAME=fruit
```

Renaming the resulting binary will also work, the name is not being used anywhere in the code, just for the binary output name.

The `CLAP_SDK_ROOT` and `VST3_SDK_ROOT` arguments can be omitted if the SDKs are present in the **parent folder**, the **same base folder** or in a **./libs folder**.
In this case the cmake script will detect and use them automatically.

You can also add the wrapper(s) to your existing project. Assume you've added the `clap-wrapper` repository into a `libs` subdirectory, you can just add these lines to your `CMakeLists.txt` file:

```cmake
# this assumes that ${PROJECT_NAME} is set to the actual plugin final filename 
set(CLAP_WRAPPER_OUTPUT_NAME ${PROJECT_NAME})
add_subdirectory(libs/clap-wrapper )
```

## How to use

The VST3 binary can be placed into the VST3 plugin folders. When the VST3 host opens the plugin, the wrapper will try to find a CLAP plugin
with the exact same name in the CLAP folders. If the binary is placed as `{VST3-Pluginfolder}/vendorname/plugin.vst3` it will look 
at `{CLAP-Pluginfolder}/plugin.clap` and also `{CLAP-Pluginfolder}/vendorname/plugin.clap`. It automatically translates all functions to VST3 then.

## But the vendor specifics I am using...

The wrapper will only use CLAP features and should be sufficent for 99% of the plugins out there, but sometimes vendor specific contracts have to be exposed. Therefore there will be clap extensions to optionally access things like property inquiries. (TODO: naming of those extensions). If present, the wrapper will call those extensions and pass information into and outof the plugin.
There are already VST3 specific extension to support the declaration of already published VST3 UUIDs, number of MIDI channels and other things.

As a plugin developer, you can use the extensions by adding the `clap-wrapper-extensions` library by adding this to your own CMakeLists.txt:

```cmake
add_subdirectory("clap-wrapper" EXCLUDE_FROM_ALL)
target_link_libraries(${project} PRIVATE clap-wrapper-extensions)
```

## But...

..yes, you're probably right - lets discuss. :)
