add_executable(floserver_tests
    test_apache_settings.cpp
    test_app_runner.cpp
    test_builtin.cpp
    test_certificates.cpp
    test_downloader.cpp
    test_encoding.cpp
    test_ipc.cpp
    test_main.cpp
    test_manifest.cpp
    test_mariadb.cpp
    test_mariadb_settings.cpp
    test_database_service.cpp
    test_ports.cpp
    test_project_config.cpp
    test_project_registry.cpp
    test_project_service.cpp
    test_protocol.cpp
    test_request_log.cpp
    test_runtime_service.cpp
    test_runtime_store.cpp
    test_service.cpp
    test_sha256.cpp
    test_php_settings.cpp
    test_phpmyadmin.cpp
    test_shim.cpp
    test_integration.cpp
    test_site_plan.cpp
    test_shared_sites.cpp
    test_ui_model.cpp
    test_ui_database.cpp
    test_ui_general.cpp
    test_web_service.cpp
    test_web_settings.cpp
    test_tar.cpp
    test_templates.cpp
    test_version.cpp
    test_zip.cpp)
# test_service.cpp runs the real floserverd.
add_dependencies(floserver_tests floserverd floserver_shim)
target_compile_definitions(floserver_tests PRIVATE FLOSERVER_TEST_BIN_DIR="$<TARGET_FILE_DIR:floserverd>")
target_link_libraries(floserver_tests PRIVATE floserver_core floserver_ui_model floserver_third_party floserver_monocypher floserver_miniz)
if(WIN32)
    target_link_libraries(floserver_tests PRIVATE ws2_32 crypt32)
else()
    # Certificates are checked with OpenSSL's own verifier where it's there.
    find_package(OpenSSL 3.0 COMPONENTS Crypto)
    if(OpenSSL_FOUND)
        target_link_libraries(floserver_tests PRIVATE OpenSSL::Crypto)
        target_compile_definitions(floserver_tests PRIVATE FLOSERVER_TEST_OPENSSL)
    endif()
endif()
floserver_code_rules(floserver_tests)
add_test(NAME floserver_tests COMMAND floserver_tests)
# A hung test fails after 5 minutes instead of holding CI for hours.
set_tests_properties(floserver_tests PROPERTIES TIMEOUT 300)

# The manifest tool's build logic, when the tools are built.
if(TARGET floserver_manifest_build)
    target_sources(floserver_tests PRIVATE test_manifest_build.cpp test_publish.cpp)
    target_link_libraries(floserver_tests PRIVATE floserver_manifest_build)
endif()
