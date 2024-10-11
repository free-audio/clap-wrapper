# clap-wrapper CLI

A command-line tool to wrap CLAP plugins into other plugin formats.

At the moment, only thick wrappers for AUv2 and VST3 are supported.  
These thick wrappers bundle the CLAP plugin inside them, so they don't require the CLAP plugin to be installed on the
user's machine additionally.

Support for thin wrappers (i.e. plugins that don't contain the CLAP and load it from the user's file system),
as well as support for Standalone wrapping (which can only be implemented using thin wrapping on Windows and Linux) is
planned.

## Prerequisites

Before using clap-wrapper CLI, ensure you have the following installed on your system:

- CMake (version 3.15 or higher)

## Usage

The general usage of clap-wrapper CLI follows the pattern:

```
clap-wrapper-cli [options]
```

### Options

| Option                                  | Description                               | Default                                                   |
|-----------------------------------------|-------------------------------------------|-----------------------------------------------------------|
| `-i, --input <path>`                    | Path to the input .clap file or directory | (Required)                                                |
| `-o, --output_dir <path>`               | Output directory for wrapped plugins      | Current working directory                                 |
| `-b, --build_dir <path>`                | Build directory for CMake                 | `wrap-build` in current working directory                 |
| `--au`                                  | Build Audio Unit wrapper                  | Off                                                       |
| `--vst3`                                | Build VST3 wrapper                        | Off                                                       |
| `--au_manufacturer_code <code>`         | AU manufacturer code (4 letters)          | Read from `clap_plugin_info_as_auv2` extension if present |
| `--au_manufacturer_name <name>`         | AU manufacturer display name              | Read from `clap_plugin_info_as_auv2` extension if present |
| `-r, --clap_wrapper_github_repo <repo>` | GitHub repository for clap-wrapper        | "free-audio/clap-wrapper"                                 |
| `-t, --clap_wrapper_git_tag <tag>`      | Git tag or branch for clap-wrapper        | "main"                                                    |
| `--cmake <path>`                        | Path to CMake binary to use for wrapping  | "cmake"                                                   |

Note: At least one of `--au` or `--vst3` must be specified.

The AU manufacturer code and name are read from the CLAP plugin's `clap_plugin_info_as_auv2` extension if it's
implemented. If the extension is not present, the CLI options `--au_manufacturer_code` and `--au_manufacturer_name`
become mandatory for AU wrapping.

Example usage:

```shell
clap-wrapper-cli -i /path/to/my/plugin.clap -o /output/directory --vst3 --au --au_manufacturer_code Abcd --au_manufacturer_name "My Company"
```

This command will wrap the CLAP plugin located at `/path/to/my/plugin.clap` into both VST3 and AU formats, placing the
output in `/output/directory`. If the plugin implements the `clap_plugin_info_as_auv2` extension, the manufacturer code
and name from the extension will be used unless overridden by the CLI options.

## AU Wrapping

Audio Unit (AU) wrapping is only supported on macOS.  
When wrapping a CLAP plugin as an AU, information about the plugin
manufacturer needs to be supplied.

The clap-wrapper CLI handles manufacturer information as follows:

- Command-line options:
    - You can provide the manufacturer information using the `--au_manufacturer_code` and `--au_manufacturer_name`
      options.
    - The manufacturer code must be exactly 4 characters long, starting with an uppercase letter followed by lowercase
      letters and/or numbers.
    - If provided, these values will be used for the AU wrapper, regardless of whether the CLAP plugin implements the
      `clap_plugin_info_as_auv2` extension.

- The `clap_plugin_info_as_auv2` extension in the CLAP plugin:
    - If the CLAP plugin implements this extension, it can provide the manufacturer code and name directly from the
      plugin.
    - These values will be used only if the corresponding command-line options are not provided.

- If neither the command-line options nor the `clap_plugin_info_as_auv2` extension provide the manufacturer information,
  the AU wrapping process will fail.

Implementing the `clap_plugin_info_as_auv2` extension is recommended to provide default manufacturer information.
This ensures that users can wrap the plugin as an AU without needing to specify the manufacturer details manually,
but still allows for overriding if needed.

## Installation

Currently, there are no pre-built binaries available for download.
To use clap-wrapper CLI, you'll need to build it from source.

## Building from Source

To compile this project yourself, follow these steps:

1. Clone the repository:
   ```
   git clone https://github.com/free-audio/clap-wrapper.git
   cd clap-wrapper
   ```

2. Create a build directory and navigate to it:
   ```
   mkdir build
   cd build
   ```

3. Run CMake to configure the project and build the CLI:
   ```
   cmake ..
   cmake --build . --target clap-wrapper-cli
   ```

After successful compilation, you should have the `clap-wrapper-cli` executable in the `cli` subfolder of your build
directory.
