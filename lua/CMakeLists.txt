# Bundled Lua 5.4 — compiled as static library.
# Used by spkg for embedded scripting.

cmake_minimum_required(VERSION 3.5)

set(LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Core Lua sources (exclude luac.c — standalone compiler, not needed)
set(LUA_SOURCES
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c
    ldblib.c ldebug.c ldo.c ldump.c lfunc.c lgc.c linit.c
    liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c
    lopcodes.c loslib.c lparser.c lstate.c lstring.c lstrlib.c
    ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c
)

add_library(lua_54 STATIC ${LUA_SOURCES})

target_include_directories(lua_54 PUBLIC
    $<BUILD_INTERFACE:${LUA_DIR}>
)

target_compile_definitions(lua_54 PRIVATE
    LUA_COMPAT_5_3
)

# Lua needs math library on POSIX
if(UNIX)
    target_link_libraries(lua_54 PUBLIC m)
endif()

# Allow using lua_54 without install/export issues
set_target_properties(lua_54 PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)