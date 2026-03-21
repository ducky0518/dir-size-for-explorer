add_executable(DirSizeSvc
    main.cpp
    service_control.cpp
    scanner.cpp
    throttle.cpp
    ipc_server.cpp
    change_journal.cpp
)

target_link_libraries(DirSizeSvc PRIVATE
    DirSizeCommon
    advapi32
)

# Service needs to link to kernel32 for DeviceIoControl etc.
# (automatically linked, but be explicit)
target_link_libraries(DirSizeSvc PRIVATE kernel32)
