# lua/CMakeLists.txt — embedded Lua 5.4 static library.
# Called from spkg's parent CMakeLists as add_subdirectory(lua).

cmake_minimum_required(VERSION 3.14)
project(lua_embedded LANGUAGES C)

file(GLOB LUA_CORE *.c)
# Exclude luac.c, lua.c, ltests.c — they are standalone programs/test code
list(REMOVE_ITEM LUA_CORE "${CMAKE_CURRENT_SOURCE_DIR}/lua.c")
list(REMOVE_ITEM LUA_CORE "${CMAKE_CURRENT_SOURCE_DIR}/luac.c")
list(REMOVE_ITEM LUA_CORE "${CMAKE_CURRENT_SOURCE_DIR}/ltests.c")
list(REMOVE_ITEM LUA_CORE "${CMAKE_CURRENT_SOURCE_DIR}/onelua.c")

add_library(lua54 STATIC ${LUA_CORE})

target_include_directories(lua54 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(lua54 PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(UNIX)
    target_link_libraries(lua54 PUBLIC m dl)
endif()