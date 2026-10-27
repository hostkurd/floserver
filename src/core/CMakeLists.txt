# The shared core: every rule and decision lives here, no UI code (D1).

configure_file(build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/floserver/core/build_info.h @ONLY)

add_library(floserver_core STATIC
    src/builtin.cpp
    src/encoding.cpp
    src/manifest.cpp
    src/ports.cpp
    src/project_config.cpp
    src/sha256.cpp
    src/version.cpp)

target_include_directories(floserver_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(floserver_core PRIVATE floserver_third_party floserver_monocypher)
if(WIN32)
    target_link_libraries(floserver_core PRIVATE ws2_32)
endif()
floserver_code_rules(floserver_core)
