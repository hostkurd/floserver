# The window's model: words in three languages, rows, colours. No Windows
# code, so it's built and tested on every platform.
add_library(floserver_ui_model STATIC
    model/theme.cpp
    model/ui_strings.cpp
    model/view_model.cpp
    model/runtimes_page.cpp
    model/php_page.cpp
    model/ports_page.cpp
    model/database_page.cpp
    model/scrollbar.cpp
    model/apache_page.cpp
    model/general_page.cpp
    model/folders_page.cpp
    model/first_run.cpp
    model/project_menu.cpp
    model/code_editor.cpp
    model/folder_removal.cpp
    model/toast.cpp
    model/glyphs.cpp
    model/access_tree.cpp
    model/main_access.cpp
    model/pages_access.cpp
    model/settings_access.cpp
    model/backup_dialogs.cpp)
target_include_directories(floserver_ui_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Core for paths.h's file helpers (window.json is read and written here, testably).
target_link_libraries(floserver_ui_model PUBLIC floserver_third_party PRIVATE floserver_core)
floserver_code_rules(floserver_ui_model)

if(NOT WIN32)
    return()
endif()

enable_language(RC)

add_executable(floserver_ui WIN32
    main.cpp
    main_window.cpp
    main_window_panel.cpp
    main_window_php.cpp
    main_window_shared.cpp
    main_window_wizard.cpp
    main_window_first_run.cpp
    main_window_menu.cpp
    main_window_access.cpp
    access_provider.cpp
    canvas.cpp
    service_worker.cpp
    settings_window.cpp
    settings_php.cpp
    settings_ports.cpp
    settings_database.cpp
    settings_apache.cpp
    settings_general.cpp
    window_prefs.cpp
    code_editors.cpp
    recycle_bin.cpp
    toast_view.cpp
    settings_folders.cpp
    settings_shared.cpp
    settings_window_access.cpp
    form_parts.cpp
    scroll_bar.cpp
    window_style.cpp
    FloServer.rc)
set_target_properties(floserver_ui PROPERTIES OUTPUT_NAME FloServer)
target_link_libraries(floserver_ui PRIVATE floserver_core floserver_ui_model
    comctl32 dwmapi gdiplus shell32 ole32 oleaut32 uuid advapi32 user32 gdi32)
# The window starts floserverd the way the CLI does; build it alongside.
add_dependencies(floserver_ui floserverd)
if(MINGW)
    target_link_options(floserver_ui PRIVATE -municode)  # entry point is wWinMain
endif()
if(MSVC)
    # The manifest comes from FloServer.rc; don't let the linker add a second one.
    target_link_options(floserver_ui PRIVATE /MANIFEST:NO)
endif()
floserver_code_rules(floserver_ui)
set_target_properties(floserver_ui PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})
