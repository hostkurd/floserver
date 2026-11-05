add_executable(floserver_cli main.cpp)
target_link_libraries(floserver_cli PRIVATE floserver_core)
if(MINGW)
    target_link_options(floserver_cli PRIVATE -municode)  # entry point is wmain
endif()
floserver_code_rules(floserver_cli)
set_target_properties(floserver_cli PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})

if(WIN32)
    # Windows ignores case in file names, so floserver.exe would be the same
    # file as FloServer.exe (the window) in the same folder: on Windows the
    # command line is flo.exe only, and `floserver` opens the window (5 Nov).
    set_target_properties(floserver_cli PROPERTIES OUTPUT_NAME flo)
else()
    set_target_properties(floserver_cli PROPERTIES OUTPUT_NAME floserver)
    # The short name: the same program as `flo` (decided 28 Oct; was `fls`).
    add_custom_command(TARGET floserver_cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:floserver_cli>
            $<TARGET_FILE_DIR:floserver_cli>/flo${CMAKE_EXECUTABLE_SUFFIX}
        VERBATIM)
endif()
