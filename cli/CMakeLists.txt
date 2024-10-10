project(clap-wrapper-cli)
add_executable(${PROJECT_NAME}
        src/main.cpp
)

target_include_directories(${PROJECT_NAME}  PRIVATE ../libs/fmt )
target_include_directories(${PROJECT_NAME}  PRIVATE ../libs/CLI11 )
target_link_libraries(${PROJECT_NAME} PRIVATE clap-wrapper-compile-options-public)
target_link_libraries(${PROJECT_NAME} PRIVATE clap-wrapper-shared-detail)
