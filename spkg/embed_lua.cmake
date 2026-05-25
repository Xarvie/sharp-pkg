# embed_lua.cmake INPUT OUTPUT VARNAME
# Reads a file and generates a C header with xxd-like format but predictable
# symbol names based on VARNAME rather than the file path.
#
# Usage: cmake -DINPUT=file.lua -DOUTPUT=file.lua.h -DVARNAME=myvar -P embed_lua.cmake

file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_len)

# Convert hex string to C array format: every 2 hex chars -> "0xNN, "
set(c_array "")
math(EXPR num_bytes "${hex_len} / 2")
set(col 0)
set(i 0)
while(i LESS hex_len)
    string(SUBSTRING "${content}" ${i} 2 byte_hex)
    math(EXPR i "${i} + 2")
    set(c_array "${c_array}0x${byte_hex}, ")
    math(EXPR col "${col} + 1")
    if(col EQUAL 12)
        set(c_array "${c_array}\n  ")
        set(col 0)
    endif()
endwhile()

# Remove trailing comma-space-newline
string(STRIP "${c_array}" c_array)
string(REGEX REPLACE ",$" "" c_array "${c_array}")

file(WRITE "${OUTPUT}"
"unsigned char ${VARNAME}[] = {\n  ${c_array}\n};\n"
"unsigned int ${VARNAME}_len = ${num_bytes};\n"
)