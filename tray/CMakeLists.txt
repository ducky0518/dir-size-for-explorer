add_executable(DirSizeTray WIN32
    main.cpp
    settings_dialog.cpp
    resource.rc
)

# Single source of truth for the displayed version (About tab): the
# top-level project() version. Previously hardcoded in the dialog, where
# it silently fell out of sync with the installer version.
target_compile_definitions(DirSizeTray PRIVATE
    "DIRSIZE_VERSION_WSTR=L\"${PROJECT_VERSION}\"")

target_link_libraries(DirSizeTray PRIVATE
    DirSizeCommon
    DirSizeEngine
    shell32
    ole32
    comctl32
    shlwapi
)

# Ensure the .ico file exists for the resource compiler.
# If it doesn't, create a placeholder (the real icon would be designed later).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dirsize.ico")
    message(WARNING "dirsize.ico not found - tray icon will use a placeholder. "
                    "Add a proper .ico file to tray/ for the system tray icon.")
endif()
