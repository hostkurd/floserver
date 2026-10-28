# miniz 3.0.2 (MIT), unchanged: unpacks the runtimes' .zip files (D5). Built
# without our warning rules. FloServer does its own file I/O (wide paths on
# Windows), so miniz's stdio and time functions are left out.
add_library(floserver_miniz STATIC miniz.c)
target_include_directories(floserver_miniz SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(floserver_miniz PUBLIC
    MINIZ_NO_STDIO MINIZ_NO_TIME MINIZ_NO_ZLIB_COMPATIBLE_NAMES)
set_target_properties(floserver_miniz PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_definitions(floserver_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(FLOSERVER_SANITIZE AND NOT MSVC)
    target_compile_options(floserver_miniz PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
