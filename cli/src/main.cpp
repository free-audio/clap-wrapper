#include <iostream>
#include <string>
#include <fstream>
#include <stdexcept>
#include <CLI/CLI.hpp>
#include <fmt/format.h>

// rationale against using std::filesystem is that
// it requires macOS 10.15, but our min supported version is 10.13 atm
#ifdef _WIN32
#include <direct.h>
#define CREATE_DIRECTORY(dir) _mkdir(dir)
#define DIRECTORY_EXISTS (errno == EEXIST)
#define PATH_SEPARATOR "\\"
#define GET_CURRENT_DIR _getcwd
#else
#include <sys/stat.h>
#include <unistd.h>
#define CREATE_DIRECTORY(dir) mkdir(dir, 0777)
#define DIRECTORY_EXISTS (errno == EEXIST)
#define PATH_SEPARATOR "/"
#define GET_CURRENT_DIR getcwd
#endif

std::string get_absolute_path(const std::string& path)
{
  if (path.empty())
  {
    return "";
  }

  if (path[0] == '/' || (path.length() > 1 && path[1] == ':'))
  {
    // Path is already absolute
    return path;
  }

  char current_dir[1024];
  if (GET_CURRENT_DIR(current_dir, sizeof(current_dir)) == nullptr)
  {
    throw std::runtime_error("Failed to get current directory");
  }

  return std::string(current_dir) + PATH_SEPARATOR + path;
}

/**
 * Creates a directory if it doesn't exist.
 * @param path Path of the directory to create.
 */
void create_directory(const std::string& path)
{
  if (CREATE_DIRECTORY(path.c_str()) != 0)
  {
    if (!DIRECTORY_EXISTS)
    {
      throw std::runtime_error("Failed to create directory: " + path +
                               " Error: " + std::strerror(errno));
    }
  }
}

const char* cmake_content = R"(
cmake_minimum_required(VERSION 3.15)
project(${PROJECT_NAME})

set(CMAKE_CXX_STANDARD 17)

# Download CPM.cmake
file(
  DOWNLOAD
  https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.2/CPM.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake
  EXPECTED_HASH SHA256=C8CDC32C03816538CE22781ED72964DC864B2A34A310D3B7104812A5CA2D835D
)
include(${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM.cmake)

# Add clap-wrapper as a dependency
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON)
CPMAddPackage(
  NAME clap-wrapper
  GITHUB_REPOSITORY free-audio/clap-wrapper
  GIT_TAG main
)

if(BUILD_AU)
  # required to compile AU SDK
  enable_language(OBJC)
  enable_language(OBJCXX)

  set(AUV2_TARGET ${PROJECT_NAME}_auv2)
  add_library(${AUV2_TARGET} MODULE)
  target_add_auv2_wrapper(
      TARGET ${AUV2_TARGET}
      MACOS_EMBEDDED_CLAP_LOCATION ${CLAP_PLUGIN_PATH}

      # TODO: remove all the below once baconpaul reads it from the CLAP
      OUTPUT_NAME "${PROJECT_NAME}"
      BUNDLE_IDENTIFIER "com.yourcompany.${PROJECT_NAME}clap"
      BUNDLE_VERSION "1.0"
      MANUFACTURER_NAME "Your Company"
      MANUFACTURER_CODE "YuCu"
      SUBTYPE_CODE "${AU_SUBTYPE_CODE}"
      INSTRUMENT_TYPE "aufx"
  )

  # copy AU to output directory
  add_custom_command(TARGET ${AUV2_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
          $<TARGET_BUNDLE_DIR:${AUV2_TARGET}>
          "${OUTPUT_DIR}/${PROJECT_NAME}.component"
  )
endif()

if(BUILD_VST3)
  set(VST3_TARGET ${PROJECT_NAME}_vst3)
  add_library(${VST3_TARGET} MODULE)
  target_add_vst3_wrapper(
      TARGET ${VST3_TARGET}
      MACOS_EMBEDDED_CLAP_LOCATION ${CLAP_PLUGIN_PATH}
      WINDOWS_FOLDER_VST3 ON
      OUTPUT_NAME "${PROJECT_NAME}"
  )

  # copy VST3 to output directory
  add_custom_command(TARGET ${VST3_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
          $<TARGET_BUNDLE_DIR:${VST3_TARGET}>
          "${OUTPUT_DIR}/${PROJECT_NAME}.vst3"
  )
endif()

if(BUILD_STANDALONE)
  set(STANDALONE_TARGET ${PROJECT_NAME}_standalone)
  add_executable(${STANDALONE_TARGET})
  target_add_standalone_wrapper(
      TARGET ${STANDALONE_TARGET}
      MACOS_EMBEDDED_CLAP_LOCATION ${CLAP_PLUGIN_PATH}
      OUTPUT_NAME "${PROJECT_NAME}"
  )

  # copy Standalone to output directory
  if(APPLE)
    add_custom_command(TARGET ${STANDALONE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            $<TARGET_BUNDLE_DIR:${STANDALONE_TARGET}>
            "${OUTPUT_DIR}/${PROJECT_NAME}.app"
    )
  elseif(WIN32)
    add_custom_command(TARGET ${STANDALONE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:${STANDALONE_TARGET}>
            "${OUTPUT_DIR}/${PROJECT_NAME}.exe"
    )
  else()
    add_custom_command(TARGET ${STANDALONE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:${STANDALONE_TARGET}>
            "${OUTPUT_DIR}/${PROJECT_NAME}"
    )
  endif()
endif()
)";


void write_cmakelists(const std::string& output_dir)
{
  std::string cmake_path = output_dir + "/CMakeLists.txt";
  std::ofstream cmake_file(cmake_path);
  if (!cmake_file)
  {
    throw std::runtime_error("Failed to create CMakeLists.txt");
  }

  cmake_file << cmake_content;
}

int main(int argc, char** argv)
{
  CLI::App app{"CLAP Wrapper CLI"};

  std::string input_path;
  std::string output_dir = ".";
  std::string build_dir = "";
  bool build_au = false;
  bool build_vst3 = false;
  bool build_standalone = false;

  app.add_option("-i,--input", input_path, "Input .clap file or directory")->required();
  app.add_option("-o,--outputDir", output_dir, "Output directory");
  app.add_option("-b,--buildDir", build_dir, "Build directory for CMake");
  app.add_flag("--au", build_au, "Build Audio Unit wrapper");
  app.add_flag("--vst3", build_vst3, "Build VST3 wrapper");
  app.add_flag("--standalone", build_standalone, "Build standalone application");

  CLI11_PARSE(app, argc, argv);

  if (!build_au && !build_vst3 && !build_standalone)
  {
    throw std::runtime_error("At least one output format must be specified");
  }

  // resolve paths to absolute
  input_path = get_absolute_path(input_path);
  output_dir = get_absolute_path(output_dir);

  if (build_dir.empty())
  {
    // default to adding a build dir in the working directory
    build_dir = get_absolute_path("wrap-build");
  }
  else
  {
    build_dir = get_absolute_path(build_dir);
  }

  try
  {
    // create the output directory if it doesn't exist
    create_directory(output_dir);

    // create build directory
    create_directory(build_dir);

    // write CMakeLists.txt
    write_cmakelists(build_dir);

    // extract project name from input path
    std::string::size_type pos = input_path.find_last_of("/\\");
    std::string clap_filename = (pos == std::string::npos) ? input_path : input_path.substr(pos + 1);
    std::string project_name = clap_filename.substr(0, clap_filename.find_last_of('.'));

    // construct cmake command with variables supplied
    std::string cmake_cmd = fmt::format(
        "cmake . "
        "-DPROJECT_NAME=\"{}\" "
        "-DCLAP_PLUGIN_PATH=\"{}\" "
        "-DOUTPUT_DIR=\"{}\" "
        "-DBUILD_AU={} "
        "-DBUILD_VST3={} "
        "-DBUILD_STANDALONE={} "
        "-DAU_SUBTYPE_CODE=\"{}\"",
        project_name, input_path, output_dir, build_au ? "ON" : "OFF", build_vst3 ? "ON" : "OFF",
        build_standalone ? "ON" : "OFF", project_name.substr(0, 4));

    // run CMake
    cmake_cmd = fmt::format("cd \"{}\" && {} && cmake --build . --config Release", build_dir, cmake_cmd);

    int result = std::system(cmake_cmd.c_str());
    if (result != 0)
    {
      throw std::runtime_error("CMake build failed");
    }

    std::cout << "Wrapping completed successfully. Output files are in: " << output_dir << std::endl;
  }
  catch (const std::exception& e)
  {
    std::cerr << "Error during wrapping: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}