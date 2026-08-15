cmake_minimum_required(VERSION 3.20)

project(dsh_tui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep the vendored third-party trees lean: this project only needs the
# libraries, not their test/example/GPU surface.
set(EXECUTOR_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXECUTOR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(EXECUTOR_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(EXECUTOR_ENABLE_GPU OFF CACHE BOOL "" FORCE)
set(EXECUTOR_ENABLE_CUDA OFF CACHE BOOL "" FORCE)
set(EXECUTOR_ENABLE_OPENCL OFF CACHE BOOL "" FORCE)

set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/FTXUI EXCLUDE_FROM_ALL)
add_subdirectory(third_party/executor EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)

add_executable(dsh_tui
  src/main.cpp
  src/dsh_launcher.cpp
  src/ds_state.cpp
  src/ds_bridge.cpp
  src/ds_protocol.cpp
)

target_compile_definitions(dsh_tui PRIVATE
  DSH_TUI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_include_directories(dsh_tui PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/executor/include
)

target_link_libraries(dsh_tui PRIVATE
  ftxui::ftxui
  executor::executor
  Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(dsh_tui PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(GNUInstallDirs)
install(TARGETS dsh_tui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
