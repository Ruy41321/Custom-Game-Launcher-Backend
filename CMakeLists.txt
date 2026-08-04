cmake_minimum_required(VERSION 3.22)

project(CustomGameLauncherBackend
        VERSION 0.1.0
        DESCRIPTION "REST API server for the Custom Game Launcher"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(LAUNCHER_BUILD_TESTS "Build the automated test suite" ON)

# ---------------------------------------------------------------------------
# Dependencies (provided by vcpkg manifest mode, see vcpkg.json)
# ---------------------------------------------------------------------------
find_package(Drogon CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(jwt-cpp CONFIG REQUIRED)

# libpq is used directly by the migration runner. Drogon's PostgreSQL backend cannot send
# multi-statement scripts once libpq is new enough for pipeline mode; see
# src/migrations/MigrationRunner.h.
find_package(PostgreSQL REQUIRED)

# The libsodium port has exported two different config names over time.
find_package(unofficial-sodium CONFIG QUIET)
if(TARGET unofficial-sodium::sodium)
    set(LAUNCHER_SODIUM_TARGET unofficial-sodium::sodium)
else()
    find_package(libsodium CONFIG REQUIRED)
    set(LAUNCHER_SODIUM_TARGET libsodium::libsodium)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings, shared by every target we own
# ---------------------------------------------------------------------------
add_library(launcher_warnings INTERFACE)
if(MSVC)
    target_compile_options(launcher_warnings INTERFACE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(launcher_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Werror -Wshadow -Wnon-virtual-dtor)
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher/Version.h"
    @ONLY)

# ---------------------------------------------------------------------------
# launcher_core — all application code. The executable is a thin shell around
# it so that the test binaries link exactly the same objects.
#
# This is an OBJECT library on purpose. Drogon registers controllers through
# static initialisers; in a STATIC library the linker discards those objects
# because nothing references them, and every route silently 404s. Linking the
# objects directly keeps the registrations alive without --whole-archive.
# ---------------------------------------------------------------------------
file(GLOB_RECURSE LAUNCHER_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM LAUNCHER_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# The admin console is embedded in the binary rather than served from a directory: the
# deployed image then has no path to mount, nothing can get out of step with the API it talks
# to, and the integration tests exercise the same bytes a deployment serves. The source stays
# a real .html file, which is the part that matters for editing it.
#
# CONFIGURE_DEPENDS on the directory is what makes a change to the HTML re-run CMake; file(READ)
# alone happens once at configure time and would otherwise leave a stale page compiled in.
set(LAUNCHER_ADMIN_UI_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/admin/ui/index.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${LAUNCHER_ADMIN_UI_SOURCE}")
file(READ "${LAUNCHER_ADMIN_UI_SOURCE}" LAUNCHER_ADMIN_UI_HTML)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AdminUi.cpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/AdminUi.cpp"
    @ONLY)
list(APPEND LAUNCHER_CORE_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/generated/AdminUi.cpp")

add_library(launcher_core OBJECT ${LAUNCHER_CORE_SOURCES})

target_include_directories(launcher_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_link_libraries(launcher_core
    PUBLIC
        Drogon::Drogon
        spdlog::spdlog
        OpenSSL::Crypto
        PostgreSQL::PostgreSQL
        jwt-cpp::jwt-cpp
        ${LAUNCHER_SODIUM_TARGET}
    PRIVATE
        launcher_warnings)

add_executable(launcher-api "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_link_libraries(launcher-api PRIVATE launcher_core launcher_warnings)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LAUNCHER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS launcher-api RUNTIME DESTINATION bin)
