# FloServer Publisher (D18): the window that checks, builds and signs the next
# runtime list. For HostKurd's release routine; not shipped to users.
enable_language(RC)

add_executable(floserver_publisher WIN32 main.cpp Publisher.rc)
set_target_properties(floserver_publisher PROPERTIES OUTPUT_NAME floserver-publisher)
target_include_directories(floserver_publisher PRIVATE ${PROJECT_SOURCE_DIR}/src/ui-win32)
target_link_libraries(floserver_publisher PRIVATE floserver_manifest_build floserver_core
    floserver_third_party floserver_monocypher comctl32 comdlg32 shell32 ole32 user32 gdi32)
if(MINGW)
    target_link_options(floserver_publisher PRIVATE -municode)  # entry point is wWinMain
endif()
if(MSVC)
    # The manifest comes from Publisher.rc; don't let the linker add a second one.
    target_link_options(floserver_publisher PRIVATE /MANIFEST:NO)
endif()
floserver_code_rules(floserver_publisher)
# Next to the other programs, so the CI build carries it.
set_target_properties(floserver_publisher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})
