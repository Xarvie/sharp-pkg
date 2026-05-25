# Bundled mbedTLS 2.x — compiled directly from source, no Python dependency.
# Produces a single object library: mbedtls_bundled.
# Used by libgit2 when system mbedTLS is not found.

cmake_minimum_required(VERSION 3.5)

set(MBEDTLS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

file(GLOB MBEDTLS_SOURCES library/*.c)

add_library(mbedtls_bundled OBJECT ${MBEDTLS_SOURCES})

target_include_directories(mbedtls_bundled
    PUBLIC
        $<BUILD_INTERFACE:${MBEDTLS_DIR}/include>
    PRIVATE
        ${MBEDTLS_DIR}/library
)

# mbedTLS requires C99 (C++ style comments, inline, etc.)
set_target_properties(mbedtls_bundled PROPERTIES C_STANDARD 99)
set_target_properties(mbedtls_bundled PROPERTIES C_STANDARD_REQUIRED ON)