# Code rules shared with FloCMS Studio: warnings are errors, optional sanitizers
# and clang-tidy. Apply them to a target with floserver_code_rules(<target>).

# Third-party headers are included as SYSTEM so their warnings don't count.
add_library(floserver_third_party INTERFACE)
target_include_directories(floserver_third_party SYSTEM INTERFACE
    ${PROJECT_SOURCE_DIR}/third_party)

if(FLOSERVER_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy-18 clang-tidy REQUIRED)
    set(FLOSERVER_CLANG_TIDY_COMMAND "${CLANG_TIDY_EXE}")
    # CI keeps the files that passed between runs (tools/ci/clang_tidy_cache.py).
    if(FLOSERVER_CLANG_TIDY_CACHE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(FLOSERVER_CLANG_TIDY_COMMAND
            "${Python3_EXECUTABLE};${PROJECT_SOURCE_DIR}/tools/ci/clang_tidy_cache.py;${CLANG_TIDY_EXE}")
    endif()
endif()

function(floserver_code_rules target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
        target_compile_definitions(${target} PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
            -Wnull-dereference -Wimplicit-fallthrough -Werror)
        if(WIN32)
            target_compile_definitions(${target} PRIVATE
                UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
        endif()
    endif()

    if(FLOSERVER_SANITIZE AND NOT MSVC)
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()

    if(FLOSERVER_CLANG_TIDY)
        set_target_properties(${target} PROPERTIES
            CXX_CLANG_TIDY "${FLOSERVER_CLANG_TIDY_COMMAND};--warnings-as-errors=*")
    endif()
endfunction()
