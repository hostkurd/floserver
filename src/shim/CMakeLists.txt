# The terminal shim (D10): one program, copied as php, node, npm... by
# `flo shims install`.
add_executable(floserver_shim main.cpp)
set_target_properties(floserver_shim PROPERTIES OUTPUT_NAME floserver-shim)
target_link_libraries(floserver_shim PRIVATE floserver_core)
if(MINGW)
    target_link_options(floserver_shim PRIVATE -municode)  # entry point is wmain
endif()
floserver_code_rules(floserver_shim)
set_target_properties(floserver_shim PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${FLOSERVER_BIN_DIR})
