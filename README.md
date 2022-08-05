# clap-wrapper
Wrappers for other plugin formats to the CLAP audio plugin format (see https://github.com/free-audio/clap)

## General Idea

This project will provide a library that is statically linked to a CLAP plugin. It exposes the entrypoints for different Audio Plugin standards and their interfaces and maps them quite immediately to the CLAP in the same binary (this might be dynamic later on). The code locates the CLAP entrypoint and uses it from a host perspective.

The CLAP community calls this a PALCer.

## Status

Currently this only works on Windows and it will always load the clap-saw example CLAP. One of the next steps is to use the clap-scanner from clap-info and make this truely cross platform.

## But the vendor specifics I am using...

The wrapper will only use CLAP features and should be sufficent for 99% of the plugins out there, but sometimes vendor specific contracts have to be exposed. Therefore there will be clap extensions to optionally access things like property inquiries. (TODO: naming of those extensions). If present, the wrapper will call those extensions and pass information into and outof the plugin.

## But...

..yes, you're probably right - lets discuss. :)
