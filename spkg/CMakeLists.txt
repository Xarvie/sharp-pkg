# spkg/CMakeLists.txt — Sharp Package Manager
#
# Builds a single statically-linked binary:
#   spkg = main.o + native.o + liblua54.a + libgit2.a (pre-built)
#
# Lua scripts under scripts/ are embedded via embed_lua.cmake.
# Configuration is pure Lua (no JSON or TOML).
#
# Requires: libgit2 pre-built in ../libgit2/build/

cmake_minimum_required(VERSION 3.14)
project(spkg LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── 1. Embed Lua scripts → C source files ────────────────────────

set(LUA_SCRIPTS
    scripts/spkg_init.lua
    scripts/spkg_build.lua
    scripts/spkg_resolve.lua
    scripts/spkg_lock.lua
    scripts/spkg_fetch.lua
)

foreach(script ${LUA_SCRIPTS})
    get_filename_component(sname ${script} NAME)
    set(out_c "${CMAKE_CURRENT_BINARY_DIR}/${sname}.c")
    string(REGEX REPLACE "[.]" "_" varname "scripts_${sname}")
    add_custom_command(
        OUTPUT ${out_c}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}"
        COMMAND ${CMAKE_COMMAND}
            -DINPUT="${CMAKE_CURRENT_SOURCE_DIR}/${script}"
            -DOUTPUT="${out_c}"
            -DVARNAME="${varname}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/embed_lua.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${script}"
                "${CMAKE_CURRENT_SOURCE_DIR}/embed_lua.cmake"
        COMMENT "Embedding ${sname}"
    )
    list(APPEND EMBED_SOURCES ${out_c})
endforeach()

# ── 2. Lua 5.4 ────────────────────────────────────────────────────────

if(NOT TARGET lua54)
    add_subdirectory(lua ${CMAKE_CURRENT_BINARY_DIR}/lua)
endif()

# ── 3. libgit2 (pre-built) ────────────────────────────────────────────

set(LIBGIT2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libgit2")

if(EXISTS "${LIBGIT2_DIR}/build/libgit2.a")
    message(STATUS "Found libgit2 at ${LIBGIT2_DIR}/build")
    add_library(libgit2_static STATIC IMPORTED)
    set_target_properties(libgit2_static PROPERTIES
        IMPORTED_LOCATION "${LIBGIT2_DIR}/build/libgit2.a"
    )
    set(LIBGIT2_INCLUDES "${LIBGIT2_DIR}/include")
    set(LIBGIT2_LIBS libgit2_static)
else()
    message(WARNING "libgit2 not pre-built. Run:")
    message(WARNING "  cd sharp-pkg/libgit2 && cmake -B build -DBUILD_CLI=OFF && cmake --build build")
    set(LIBGIT2_INCLUDES "")
    set(LIBGIT2_LIBS "")
endif()

# ── 4. spkg executable ────────────────────────────────────────────────

add_executable(spkg
    main.c
    native.c
    mongoose.c
    ${EMBED_SOURCES}
)

target_include_directories(spkg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/lua
    ${LIBGIT2_INCLUDES}
)

target_link_libraries(spkg PRIVATE lua54 ${LIBGIT2_LIBS})

if(UNIX)
    target_link_libraries(spkg PRIVATE pthread)
endif()

# ── install ───────────────────────────────────────────────────────────

install(TARGETS spkg RUNTIME DESTINATION bin)

# ── 5. spkg-node executable ─────────────────────────────────────────

add_executable(spkg-node
    node.c
    mongoose.c
)

target_include_directories(spkg-node PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

install(TARGETS spkg-node RUNTIME DESTINATION bin)