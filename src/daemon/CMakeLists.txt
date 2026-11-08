add_executable(floserverd main.cpp)
target_link_libraries(floserverd PRIVATE floserver_core)
floserver_code_rules(floserverd)
set_target_properties(floserverd PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})

# On Windows a Windows program, not a console one: no console window ever
# flashes when the window, flo, Start with Windows or the installer starts
# it. main() stays the entry point (MinGW's -mwindows keeps it; MSVC is told).
# What it prints reaches a console it was typed in (use_parent_console()).
if(WIN32)
    set_target_properties(floserverd PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(floserverd PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

# The Microsoft Visual C++ runtime DLLs, shipped next to floserverd: the
# programs use them, and the service copies them into each runtime it
# installs (PHP, nginx, Apache need them), so users never install the
# redistributable. One copy, no vcruntime/ folder.
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
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}
                $<TARGET_FILE_DIR:floserverd>
            VERBATIM)
    else()
        message(WARNING "The Visual C++ runtime DLLs weren't found; installed runtimes may not start")
    endif()
endif()
