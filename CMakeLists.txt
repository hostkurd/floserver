cmake_minimum_required(VERSION 3.25)

project(FloServer
    VERSION 0.1.0
    DESCRIPTION "Tiny local development environment for PHP, Node.js and databases"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FLOSERVER_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FLOSERVER_CLANG_TIDY "Run clang-tidy while compiling" OFF)
option(FLOSERVER_BUILD_TESTS "Build the unit tests" ON)

include(cmake/CodeRules.cmake)

add_subdirectory(src/core)
add_subdirectory(src/daemon)
add_subdirectory(src/cli)
if(WIN32)
    add_subdirectory(src/ui-win32)
endif()

if(FLOSERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
