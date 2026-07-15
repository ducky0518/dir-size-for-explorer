# The scan engine used to be a Windows service (DirSizeSvc.exe) running as
# LocalSystem. It is now a static library hosted by the tray app in the
# user's session, because the primary use case is scanning the user's
# (mapped) network drives:
#   - drive-letter mappings only exist in the user's logon session
#   - SMB shares authenticate with the user's credentials
#   - ReadDirectoryChangesW-based watching needs no admin rights
#
# main.cpp / service_control.cpp / change_journal.cpp are intentionally no
# longer compiled (SCM plumbing and the USN journal monitor, which required
# admin rights and only worked on local NTFS volumes).

add_library(DirSizeEngine STATIC
    scanner.cpp
    throttle.cpp
    ipc_server.cpp
    dir_watcher.cpp
    log_buffer.cpp
)

target_include_directories(DirSizeEngine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(DirSizeEngine PUBLIC
    DirSizeCommon
    advapi32
)
