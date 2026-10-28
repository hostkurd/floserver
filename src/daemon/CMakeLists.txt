add_executable(floserverd main.cpp)
target_link_libraries(floserverd PRIVATE floserver_core)
floserver_code_rules(floserverd)
set_target_properties(floserverd PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})

# The Microsoft Visual C++ runtime DLLs, shipped next to floserverd in
# vcruntime/. The service copies them into each runtime it installs (PHP,
# nginx, Apache need them), so users never install the redistributable.
# Microsoft allows this app-local use of the redistributable files. They
# come from the Visual Studio that builds FloServer, so the release build
# must use one at least as new as the newest runtime needs (Apache Lounge's
# VS18 build: Visual Studio 2026).
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
    include(InstallRequiredSystemLibraries)
    if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET floserverd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:floserverd>/vcruntime
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                $<TARGET_FILE_DIR:floserverd>/vcruntime
            VERBATIM)
    else()
        message(WARNING "The Visual C++ runtime DLLs weren't found; installed runtimes may not start")
    endif()
endif()
