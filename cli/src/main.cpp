#include <iostream>
#include <string>
#include <fstream>
#include <stdexcept>
#include <CLI/CLI.hpp>

#define FMT_HEADER_ONLY
#include <fmt/format.h>

#include "detail/clap/fsutil.h"

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
  GITHUB_REPOSITORY "${CLAP_WRAPPER_GITHUB_REPO}"
  GIT_TAG "${CLAP_WRAPPER_GIT_TAG}"
)

if(BUILD_AU)
  # required to compile AU SDK
  enable_language(OBJC)
  enable_language(OBJCXX)

  set(AUV2_TARGET ${PROJECT_NAME}_auv2)
  add_library(${AUV2_TARGET} MODULE)
  message(path=${CLAP_PLUGIN_PATH})
  target_add_auv2_wrapper(
      TARGET ${AUV2_TARGET}
      MACOSX_EMBEDDED_CLAP_LOCATION ${CLAP_PLUGIN_PATH}
      OUTPUT_NAME "${PROJECT_NAME}"
      MANUFACTURER_CODE "${AU_MANUFACTURER_CODE}"
      MANUFACTURER_NAME "${AU_MANUFACTURER_NAME}"
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
  if (APPLE)
    add_custom_command(TARGET ${VST3_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            $<TARGET_BUNDLE_DIR:${VST3_TARGET}>
            "${OUTPUT_DIR}/${PROJECT_NAME}.vst3"
    )
  else()
    add_custom_command(TARGET ${VST3_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${OUTPUT_DIR}/${PROJECT_NAME}.vst3"
    )
  endif()
endif()
)";

void write_cmakelists(const fs::path& output_dir)
{
  auto cmake_path = output_dir / "CMakeLists.txt";
  std::ofstream cmake_file(cmake_path);
  if (!cmake_file)
  {
    throw std::runtime_error("Failed to create CMakeLists.txt");
  }

  cmake_file << cmake_content;
}

/**
 * Creates a directory if it doesn't exist.
 * @param path Path of the directory to create.
 */
void create_directory_if_not_exists(const fs::path& path)
{
  try
  {
    fs::create_directories(path);
  }
  catch (fs::filesystem_error& e)
  {
    if (e.code().value() != EEXIST)
    {
      throw e;
    }
  }
}

std::string sanitize_cmake_identifier(const std::string& input)
{
  std::string result = input;

  // remove leading digits
  result.erase(0, std::min(result.find_first_not_of("0123456789"), result.size()));

  // replace invalid characters with underscores
  std::replace_if(
      result.begin(), result.end(), [](unsigned char c) { return !std::isalnum(c) && c != '_'; }, '_');

  // ensure the identifier is not empty
  if (result.empty())
  {
    result = "clap_plugin";
  }

  return result;
}

bool is_valid_au_manufacturer_code(const std::string& code)
{
  if (code.length() != 4)
  {
    return false;
  }

  if (!std::isupper(code[0]))
  {
    return false;
  }

  return std::all_of(code.begin() + 1, code.end(),
                     [](unsigned char c) { return std::islower(c) || std::isdigit(c); });
}

void error_exit(const std::string& msg, int code = 1)
{
  std::cerr << msg << std::endl;
  exit(code);
}

int main(int argc, char** argv)
{
  CLI::App app{"CLAP Wrapper CLI"};

  std::string input_path_raw;
  std::string output_dir_raw = ".";
  std::string build_dir_raw = "";
  bool build_au = false;
  bool build_vst3 = false;

  std::string au_manufacturer_code = "";
  std::string au_manufacturer_name = "";

  std::string clap_wrapper_github_repo = "free-audio/clap-wrapper";
  std::string clap_wrapper_git_tag = "main";

  app.add_option("-i,--input", input_path_raw, "Input .clap file or directory")->required();
  app.add_option("-o,--output_dir", output_dir_raw, "Output directory");
  app.add_option("-b,--build_dir", build_dir_raw, "Build directory for CMake");

  app.add_flag("--au", build_au, "Build Audio Unit wrapper");
  app.add_flag("--vst3", build_vst3, "Build VST3 wrapper");

  app.add_option("-c,--au_manufacturer_code", au_manufacturer_code, "AU manufacturer code (4 letters)");
  app.add_option("-n,--au_manufacturer_name", au_manufacturer_name, "AU manufacturer display name");

  app.add_option("-r, --clap_wrapper_github_repo", clap_wrapper_github_repo,
                 "GitHub repository for clap-wrapper (default: free-audio/clap-wrapper)");
  app.add_option("-t, --clap_wrapper_git_tag", clap_wrapper_git_tag,
                 "Git tag or branch for clap-wrapper (default: main)");

  CLI11_PARSE(app, argc, argv)

#if defined(__APPLE__)
  // validate AU manufacturer code and name
  if (build_au)
  {
    if (au_manufacturer_code.empty() != au_manufacturer_name.empty())
    {
      error_exit("Both AU manufacturer code and name must be set if one is provided");
    }
    if (!au_manufacturer_code.empty() && !is_valid_au_manufacturer_code(au_manufacturer_code))
    {
      error_exit(
          "AU manufacturer code must be exactly 4 characters long, and start with an uppercase letter "
          "followed only by lowercase letters and/or numbers");
    }
  }
#else
  if (build_au)
  {
    error_exit("AU output format is only supported on macOS");
  }
#endif

  if (!build_au && !build_vst3)
  {
    error_exit("At least one output format must be specified");
  }

  // convert paths away from raw strings
  // and resolve to absolute
  fs::path input_path = fs::absolute({fs::u8path(input_path_raw.c_str())});
  fs::path output_dir = fs::absolute({fs::u8path(output_dir_raw.c_str())});
  fs::path build_dir = fs::absolute(build_dir_raw.empty()
                                        // default to adding a build dir in the working directory
                                        ? "wrap-build"
                                        : build_dir_raw);

  try
  {
    create_directory_if_not_exists(output_dir);
    create_directory_if_not_exists(output_dir);
    create_directory_if_not_exists(build_dir);

    // write CMakeLists.txt
    write_cmakelists(build_dir);

    // extract project name from input path
    auto project_name = sanitize_cmake_identifier(input_path.stem().u8string());

    // construct cmake command with variables supplied
    std::string cmake_cmd = fmt::format(
        "cmake . "
        "-DPROJECT_NAME=\"{}\" "
        "-DCLAP_PLUGIN_PATH=\"{}\" "
        "-DOUTPUT_DIR=\"{}\" "
        "-DBUILD_AU={} "
        "-DBUILD_VST3={} "
        "-DAU_MANUFACTURER_CODE=\"{}\" "
        "-DAU_MANUFACTURER_NAME=\"{}\" "
        "-DCLAP_WRAPPER_GITHUB_REPO=\"{}\" "
        "-DCLAP_WRAPPER_GIT_TAG=\"{}\" ",
        // PROJECT_NAME
        project_name,
        // CLAP_PLUGIN_PATH
        input_path.string(),
        // OUTPUT_DIR
        output_dir.string(),
        // BUILD_AU
        build_au ? "ON" : "OFF",
        // BUILD_VST3
        build_vst3 ? "ON" : "OFF",
        // AU_MANUFACTURER_CODE
        au_manufacturer_code,
        // AU_MANUFACTURER_NAME
        au_manufacturer_name,
        // CLAP_WRAPPER_GITHUB_REPO
        clap_wrapper_github_repo,
        // CLAP_WRAPPER_GIT_TAG
        clap_wrapper_git_tag);

    // build CMake invocation
#if defined(__APPLE__)
    // on macOS, write stderr to a log file for later parsing
    auto error_log_path = build_dir / "build_error.log";
    cmake_cmd = fmt::format(
        "cd \"{}\" && "
        "{{ {} && cmake --build . --config Release ; }} 2>&1 | tee \"{}\" ; exit ${{PIPESTATUS[0]}}",
        build_dir.string(), cmake_cmd, error_log_path.string());
#else
    cmake_cmd = fmt::format("cd \"{}\" && {} && cmake --build . --config Release", build_dir.string(),
                            cmake_cmd);
#endif

    std::cout << "Invoking CMake with command" << std::endl << cmake_cmd << std::endl;

    std::cout << "----------------" << std::endl;
    int result = std::system(cmake_cmd.c_str());
    std::cout << "----------------" << std::endl;

    if (result != 0)
    {
      std::cerr << "CMake build failed with exit code " << result << std::endl;

#if defined(__APPLE__)
      if (build_au && (result / 256) == 2)
      {
        // read the stderr log to check whether the issue is a missing AU manufacturer name and/or code
        std::ifstream error_log(error_log_path);
        if (error_log.is_open())
        {
          std::string error_content((std::istreambuf_iterator<char>(error_log)),
                                    std::istreambuf_iterator<char>());
          error_log.close();

          if (error_content.find("[gain_auv2-build-helper] Error 139") != std::string::npos)
          {
            std::cerr << "AUv2 wrapping failed due to a segmentation fault in the build helper."
                      << std::endl;
            std::cerr << "This is likely caused by a missing AU manufacturer code and/or name."
                      << std::endl;
            std::cerr << "To solve this, either implement the clap_plugin_info_as_auv2 extension in "
                         "your CLAP plugin,"
                      << std::endl;
            std::cerr << "or set the --au_manufacturer_code and --au_manufacturer_name options when "
                         "calling this CLI."
                      << std::endl;
          }
        }
        else
        {
          std::cerr << "Failed to open CMakeError.log. Please check the build directory for error logs."
                    << std::endl;
        }
      }
#endif

      return result;
    }

    std::cout << "Wrapping completed successfully. Output files are in: " << output_dir << std::endl;
  }
  catch (const std::exception& e)
  {
    error_exit("Error during wrapping: " + std::string(e.what()));
    return 1;
  }

  return 0;
}