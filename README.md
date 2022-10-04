# clap-wrapper
Wrappers for other plugin formats to the CLAP audio plugin format (see https://github.com/free-audio/clap)

## General Idea

This project will provide a library that is statically linked to a CLAP plugin. It exposes the entrypoints for different Audio Plugin standards and their interfaces and maps them quite immediately to the CLAP in the same binary (this might be dynamic later on). The code locates the CLAP entrypoint and uses it from a host perspective.

The CLAP community calls this a PALCer.

## Status

Currently this works on Windows, macOS and Linux and it will always try to load a .clap with the same name as the .vst3 from the CLAP folders.
If not found, it tries to load the clap-saw example CLAP. 

Things currently missing:

- timer support on linux
- INoteExpression mapping
- CMake function to link to clap project

a first release is near.

## But the vendor specifics I am using...

The wrapper will only use CLAP features and should be sufficent for 99% of the plugins out there, but sometimes vendor specific contracts have to be exposed. Therefore there will be clap extensions to optionally access things like property inquiries. (TODO: naming of those extensions). If present, the wrapper will call those extensions and pass information into and outof the plugin.
There is already a draft for a VST3 extension to support the declaration of already published VST3 UUIDs.

## How to build

```c++
git clone https://github.com/defiantnerd/clap-wrapper.git
mkdir build
cmake -B build -DCLAP_SDK_ROOT={path to clap sdk} -DVST3_SDK_ROOT={path to vst3 sdk}
```

You can also determine the output name of the resulting VST3 plugin by using the WRAPPER_OUTPUT_NAME CMake variable.

The CLAP_SDK_ROOT and VST3_SDK_ROOT arguments can be omitted, if the SDKs are present in the parent folder, the same folder or in a ./libs folder.
In this case the cmake script will detect them accordingly.

## But...

..yes, you're probably right - lets discuss. :)
