enable_language(RC)

add_executable(floserver_ui WIN32 main.cpp FloServer.rc)
set_target_properties(floserver_ui PROPERTIES OUTPUT_NAME FloServer)
target_link_libraries(floserver_ui PRIVATE floserver_core)
if(MINGW)
    target_link_options(floserver_ui PRIVATE -municode)  # entry point is wWinMain
endif()
if(MSVC)
    # The manifest comes from FloServer.rc; don't let the linker add a second one.
    target_link_options(floserver_ui PRIVATE /MANIFEST:NO)
endif()
floserver_code_rules(floserver_ui)
