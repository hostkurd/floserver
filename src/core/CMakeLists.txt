# The shared core: every rule and decision lives here, no UI code (D1).

configure_file(build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/floserver/core/build_info.h @ONLY)

add_library(floserver_core STATIC
    src/builtin.cpp
    src/downloader.cpp
    src/encoding.cpp
    src/http.cpp
    src/http_windows.cpp
    src/ipc_common.cpp
    src/ipc_posix.cpp
    src/ipc_windows.cpp
    src/manifest.cpp
    src/nginx_config.cpp
    src/paths.cpp
    src/ports.cpp
    src/process.cpp
    src/project_config.cpp
    src/project_registry.cpp
    src/project_service.cpp
    src/site_plan.cpp
    src/protocol.cpp
    src/runtime_service.cpp
    src/runtime_store.cpp
    src/service_client.cpp
    src/sha256.cpp
    src/version.cpp
    src/zip_extract.cpp)

target_include_directories(floserver_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
# nlohmann/json is part of core's interface (protocol.h).
target_link_libraries(floserver_core PUBLIC floserver_third_party PRIVATE floserver_monocypher floserver_miniz)
if(WIN32)
    target_link_libraries(floserver_core PRIVATE ws2_32 advapi32 psapi winhttp)
endif()
floserver_code_rules(floserver_core)
