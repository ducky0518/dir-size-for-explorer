add_library(DirSizeCommon STATIC
    src/db.cpp
    src/config.cpp
    src/generation.cpp
    src/ipc.cpp
    src/guids.cpp
    src/path_utils.cpp
)

target_include_directories(DirSizeCommon PUBLIC include)

target_link_libraries(DirSizeCommon
    PUBLIC
        unofficial::sqlite3::sqlite3
        advapi32
        mpr
        shell32
        ole32
)

target_compile_definitions(DirSizeCommon PUBLIC
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    NOMINMAX
)
