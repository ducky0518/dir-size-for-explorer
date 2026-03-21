add_library(DirSizeShellExt SHARED
    dllmain.cpp
    property_handler.cpp
    context_menu.cpp
    shell_hook.cpp
    register.cpp
    DirSizeShellExt.def
)

find_path(DETOURS_INCLUDE_DIRS "detours/detours.h")
find_library(DETOURS_LIBRARY_RELEASE detours REQUIRED PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(DETOURS_LIBRARY_DEBUG detours REQUIRED PATHS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)

target_link_libraries(DirSizeShellExt PRIVATE
    DirSizeCommon
    propsys
    shlwapi
    shell32
    ole32
    oleaut32
    uuid
    $<IF:$<CONFIG:Debug>,${DETOURS_LIBRARY_DEBUG},${DETOURS_LIBRARY_RELEASE}>
)

target_include_directories(DirSizeShellExt PRIVATE ${DETOURS_INCLUDE_DIRS})

# Copy the .propdesc file next to the DLL
add_custom_command(TARGET DirSizeShellExt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/DirSizeTotalSize.propdesc"
        "$<TARGET_FILE_DIR:DirSizeShellExt>/DirSizeTotalSize.propdesc"
)
