add_executable(floserver_tests
    test_builtin.cpp
    test_encoding.cpp
    test_ipc.cpp
    test_main.cpp
    test_manifest.cpp
    test_ports.cpp
    test_project_config.cpp
    test_protocol.cpp
    test_service.cpp
    test_sha256.cpp
    test_version.cpp)
# test_service.cpp runs the real floserverd.
add_dependencies(floserver_tests floserverd)
target_compile_definitions(floserver_tests PRIVATE FLOSERVER_TEST_BIN_DIR="$<TARGET_FILE_DIR:floserverd>")
target_link_libraries(floserver_tests PRIVATE floserver_core floserver_third_party floserver_monocypher)
if(WIN32)
    target_link_libraries(floserver_tests PRIVATE ws2_32)
endif()
floserver_code_rules(floserver_tests)
add_test(NAME floserver_tests COMMAND floserver_tests)

# The manifest tool's build logic, when the tools are built.
if(TARGET floserver_manifest_build)
    target_sources(floserver_tests PRIVATE test_manifest_build.cpp)
    target_link_libraries(floserver_tests PRIVATE floserver_manifest_build)
endif()
