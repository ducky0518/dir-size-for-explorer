add_executable(DirSizeTray WIN32
    main.cpp
    settings_dialog.cpp
    resource.rc
)

target_link_libraries(DirSizeTray PRIVATE
    DirSizeCommon
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
