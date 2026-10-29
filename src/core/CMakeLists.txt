# The shared core: every rule and decision lives here, no UI code (D1).

configure_file(build_info.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/floserver/core/build_info.h @ONLY)

add_library(floserver_core STATIC
    src/builtin.cpp
    src/der.cpp
    src/child_process.cpp
    src/php_pool.cpp
    src/web_service.cpp
    src/crypto_common.cpp
    src/crypto_windows.cpp
    src/crypto_openssl.cpp
    src/certificates.cpp
    src/downloader.cpp
    src/encoding.cpp
    src/http.cpp
    src/http_windows.cpp
    src/ipc_common.cpp
    src/ipc_posix.cpp
    src/ipc_windows.cpp
    src/manifest.cpp
    src/mariadb.cpp
    src/database_service.cpp
    src/nginx_config.cpp
    src/paths.cpp
    src/ports.cpp
    src/process.cpp
    src/project_config.cpp
    src/project_registry.cpp
    src/project_service.cpp
    src/site_plan.cpp
    src/protocol.cpp
    src/trust_store.cpp
    src/runtime_service.cpp
    src/secret_store.cpp
    src/runtime_store.cpp
    src/service_client.cpp
    src/sha256.cpp
    src/version.cpp
    src/tar_extract.cpp
    src/zip_extract.cpp)

target_include_directories(floserver_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
# nlohmann/json is part of core's interface (protocol.h).
target_link_libraries(floserver_core PUBLIC floserver_third_party PRIVATE floserver_monocypher floserver_miniz)
if(WIN32)
    target_link_libraries(floserver_core PRIVATE ws2_32 advapi32 psapi winhttp bcrypt crypt32 iphlpapi)
else()
    # The local certificate authority (D7) uses the system's OpenSSL on Linux
    # and macOS. Without it the build works, but can't make certificates.
    find_package(OpenSSL 3.0 COMPONENTS Crypto)
    if(OpenSSL_FOUND)
        target_link_libraries(floserver_core PRIVATE OpenSSL::Crypto)
        target_compile_definitions(floserver_core PRIVATE FLOSERVER_HAVE_OPENSSL)
    else()
        message(WARNING "OpenSSL 3 not found: this build can't make HTTPS certificates")
    endif()
endif()
floserver_code_rules(floserver_core)
