# FloServer's setup and uninstall screens (D25).
#
#   floserver_setup_model  plain C++ (versions, modes, the feather's path data
#                          reader): built and tested on every platform;
#   floserver_setup_dll    floserver-setup.dll: every setup and uninstall
#                          screen, the feather, the icon and the licence, so
#                          FloServer.exe stays small (it loads the DLL only to
#                          uninstall);
#   floserver_setup        FloServer-Setup.exe: the download, a small launcher
#                          carrying the DLL and, when FLOSERVER_SETUP_MSI names
#                          one, FloServer's MSI (CI builds it after the MSI).
#
# The DLL and the launcher use the static C runtime and only Windows' own
# DLLs: the launcher runs before anything of FloServer's is on the PC.

add_library(floserver_setup_model STATIC
    model/setup_model.cpp
    model/svg_path.cpp)
target_include_directories(floserver_setup_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
floserver_code_rules(floserver_setup_model)

if(NOT WIN32)
    return()
endif()

enable_language(RC)

set(FLOSERVER_SETUP_MSI "" CACHE FILEPATH "FloServer's MSI, put inside FloServer-Setup.exe (CI sets it once the MSI is built)")

set(_ui ${PROJECT_SOURCE_DIR}/src/ui-win32)
set(FLOSERVER_SETUP_ICON ${_ui}/FloServer.ico)
set(FLOSERVER_SETUP_LICENCE ${PROJECT_SOURCE_DIR}/installer/License.rtf)
set(FLOSERVER_SETUP_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.manifest)
set(FLOSERVER_SETUP_VERSION_COMMAS "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")

function(floserver_setup_static_runtime target)
    set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(MINGW)
        target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
endfunction()

# --- floserver-setup.dll ---
configure_file(dll/setup_dll.rc.in ${CMAKE_CURRENT_BINARY_DIR}/setup_dll.rc @ONLY)
add_library(floserver_setup_dll SHARED
    dll/common.cpp
    dll/setup_look.cpp
    dll/setup_app.cpp
    dll/uninstall_app.cpp
    model/setup_model.cpp
    model/svg_path.cpp
    ${_ui}/model/msi_progress.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/setup_dll.rc)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/setup_dll.rc PROPERTIES
    OBJECT_DEPENDS "${FLOSERVER_SETUP_ICON};${FLOSERVER_SETUP_LICENCE}")
set_target_properties(floserver_setup_dll PROPERTIES OUTPUT_NAME floserver-setup PREFIX "")
target_include_directories(floserver_setup_dll PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/dll ${CMAKE_CURRENT_SOURCE_DIR}/include ${_ui})
target_compile_definitions(floserver_setup_dll PRIVATE FLOSERVER_SETUP_DLL_BUILD)
target_link_libraries(floserver_setup_dll PRIVATE
    comctl32 comdlg32 dwmapi gdiplus msi bcrypt shell32 ole32 advapi32 user32 gdi32)
floserver_setup_static_runtime(floserver_setup_dll)
floserver_code_rules(floserver_setup_dll)
set_target_properties(floserver_setup_dll PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})

# --- FloServer-Setup.exe ---
if(FLOSERVER_SETUP_MSI)
    file(TO_CMAKE_PATH "${FLOSERVER_SETUP_MSI}" _msi)
    set(_msi_line "FLOSERVER_MSI RCDATA \"${_msi}\"")
else()
    set(_msi_line "// No MSI inside: FLOSERVER_SETUP_MSI wasn't set (it says so when run).")
endif()
configure_file(launcher/launcher.rc.in ${CMAKE_CURRENT_BINARY_DIR}/launcher.rc.in2 @ONLY)
# The DLL's path differs per configuration: written per configuration.
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/launcher-$<CONFIG>.rc
     INPUT ${CMAKE_CURRENT_BINARY_DIR}/launcher.rc.in2)
add_executable(floserver_setup WIN32
    launcher/launcher.cpp
    model/setup_model.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/launcher-$<CONFIG>.rc)
add_dependencies(floserver_setup floserver_setup_dll)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/launcher-$<CONFIG>.rc PROPERTIES
    OBJECT_DEPENDS "$<TARGET_FILE:floserver_setup_dll>;${FLOSERVER_SETUP_MSI}")
set_target_properties(floserver_setup PROPERTIES OUTPUT_NAME FloServer-Setup)
target_include_directories(floserver_setup PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include ${_ui})
target_link_libraries(floserver_setup PRIVATE shell32 user32 advapi32)
if(MINGW)
    target_link_options(floserver_setup PRIVATE -municode)
endif()
if(MSVC)
    target_link_options(floserver_setup PRIVATE /MANIFEST:NO)
endif()
floserver_setup_static_runtime(floserver_setup)
floserver_code_rules(floserver_setup)
set_target_properties(floserver_setup PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})
