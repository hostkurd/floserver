# templates.json as a C++ expression, for the headers that build it into
# FloServer and the manifest tool (D9): a std::string_view over adjacent
# string literals of \xNN escapes, 1024 bytes each. MSVC refuses one literal
# over about 16 KB (C2026) but joins pieces up to 64 KB, and escapes never
# split a UTF-8 letter between pieces. Carriage returns from a Windows
# checkout are dropped (JSON's own strings never hold a raw one).
function(floserver_templates_literal file out_var)
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" escaped "${hex}")
    string(REPLACE "\\x0d" "" escaped "${escaped}")
    string(LENGTH "${escaped}" escaped_length)
    math(EXPR byte_count "${escaped_length} / 4")
    if(byte_count GREATER 60000)
        message(FATAL_ERROR "${file} is ${byte_count} bytes; MSVC joins string pieces only up to 64 KB")
    endif()
    set(pieces "")
    set(offset 0)
    while(offset LESS escaped_length)
        string(SUBSTRING "${escaped}" ${offset} 4096 piece)
        string(APPEND pieces "    \"${piece}\"\n")
        math(EXPR offset "${offset} + 4096")
    endwhile()
    set(${out_var} "std::string_view{\n${pieces}    , ${byte_count}}" PARENT_SCOPE)
endfunction()
