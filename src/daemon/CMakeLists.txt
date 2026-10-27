add_executable(floserverd main.cpp)
target_link_libraries(floserverd PRIVATE floserver_core)
floserver_code_rules(floserverd)
