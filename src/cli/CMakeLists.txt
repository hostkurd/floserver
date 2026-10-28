add_executable(floserver_cli main.cpp)
set_target_properties(floserver_cli PROPERTIES OUTPUT_NAME floserver)
target_link_libraries(floserver_cli PRIVATE floserver_core)
if(MINGW)
    target_link_options(floserver_cli PRIVATE -municode)  # entry point is wmain
endif()
floserver_code_rules(floserver_cli)

# The short alias from Q2: the same program under the name `fls`.
add_custom_command(TARGET floserver_cli POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:floserver_cli>
        $<TARGET_FILE_DIR:floserver_cli>/fls${CMAKE_EXECUTABLE_SUFFIX}
    VERBATIM)
set_target_properties(floserver_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})
