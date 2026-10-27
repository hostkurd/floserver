add_executable(floserver_tests
    test_main.cpp
    test_ports.cpp
    test_project_config.cpp
    test_version.cpp)
target_link_libraries(floserver_tests PRIVATE floserver_core floserver_third_party)
if(WIN32)
    target_link_libraries(floserver_tests PRIVATE ws2_32)
endif()
floserver_code_rules(floserver_tests)
add_test(NAME floserver_tests COMMAND floserver_tests)
