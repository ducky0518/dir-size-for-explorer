cmake_minimum_required(VERSION 3.24)
project(DirSizeForExplorer VERSION 2.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT (/MT). The MSI ships no VC++ redistributable, and the shell
# DLL must load into Explorer on clean machines — with the dynamic CRT the
# tray and shell extension silently failed to load wherever the matching
# redistributable wasn't already installed. Pairs with the
# x64-windows-static vcpkg triplet in CMakePresets.json (sqlite3/detours
# are linked statically too, so no sqlite3.dll is shipped either).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Source files are UTF-8. Without /utf-8, MSVC parses them in the system
# ANSI codepage and any non-ASCII characters in string literals (em dashes
# in log messages, "—" placeholders in the UI) get mangled into mojibake
# like "â€œ" at compile time.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Only support x64 builds
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Only 64-bit builds are supported")
endif()

# Find SQLite via vcpkg
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Windows SDK libraries used across multiple targets
set(DIRSIZE_WIN_LIBS
    advapi32
    shell32
    shlwapi
    ole32
    oleaut32
    propsys
    uuid
)

add_subdirectory(common)
add_subdirectory(shell-ext)
add_subdirectory(service)
add_subdirectory(tray)
add_subdirectory(tests)
