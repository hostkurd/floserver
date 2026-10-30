cmake_minimum_required(VERSION 3.25)

project(FloServer
    VERSION 0.1.0
    DESCRIPTION "Tiny local development environment for PHP, Node.js and databases"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FLOSERVER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FLOSERVER_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(FLOSERVER_CLANG_TIDY_CACHE "Skip clang-tidy on files that passed before (CI)" OFF)
option(FLOSERVER_BUILD_TESTS "Build the unit tests" ON)
option(FLOSERVER_BUILD_TOOLS "Build the release tools (manifest signing)" ON)

include(cmake/CodeRules.cmake)

# The shipped programs sit side by side in bin/, as they will when installed,
# so the CLI and the window find floserverd next to them.
set(FLOSERVER_BIN_DIR ${PROJECT_BINARY_DIR}/bin)

add_subdirectory(third_party/monocypher)
add_subdirectory(third_party/miniz)
add_subdirectory(src/core)
add_subdirectory(src/daemon)
add_subdirectory(src/cli)
add_subdirectory(src/shim)
# The window's model is built (and tested) everywhere; the window itself only on Windows.
add_subdirectory(src/ui-win32)

if(FLOSERVER_BUILD_TOOLS)
    add_subdirectory(tools/manifest-tool)
    if(WIN32)
        add_subdirectory(tools/publisher)
    endif()
endif()

if(FLOSERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
