# Monocypher 4.0.2 (BSD-2-Clause or CC0), unchanged. Only the Ed25519 check
# (RFC 8032) and SHA-512 it needs are used; built without our warning rules.
add_library(floserver_monocypher STATIC
    monocypher.c
    monocypher-ed25519.c)
target_include_directories(floserver_monocypher SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(floserver_monocypher PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON)
if(FLOSERVER_SANITIZE AND NOT MSVC)
    target_compile_options(floserver_monocypher PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
