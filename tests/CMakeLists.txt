enable_testing()

add_executable(test_db test_db.cpp)
target_link_libraries(test_db PRIVATE DirSizeCommon)
add_test(NAME DatabaseTests COMMAND test_db)

add_executable(test_scanner test_scanner.cpp)
target_link_libraries(test_scanner PRIVATE DirSizeCommon)
add_test(NAME ScannerTests COMMAND test_scanner)

add_executable(test_scan_progress test_scan_progress.cpp)
target_include_directories(test_scan_progress PRIVATE ${DETOURS_INCLUDE_DIRS})
target_link_libraries(test_scan_progress PRIVATE DirSizeEngine
    $<IF:$<CONFIG:Debug>,${DETOURS_LIBRARY_DEBUG},${DETOURS_LIBRARY_RELEASE}>)
add_test(NAME ScanProgressTests COMMAND test_scan_progress)
set_tests_properties(ScanProgressTests PROPERTIES TIMEOUT 30)

foreach(test_target test_db test_scanner)
    target_compile_options(${test_target} PRIVATE /UNDEBUG)
endforeach()

add_executable(test_ipc test_ipc.cpp)
target_link_libraries(test_ipc PRIVATE DirSizeCommon)
add_test(NAME IpcTests COMMAND test_ipc)

add_executable(manual_insert manual_insert.cpp)
target_link_libraries(manual_insert PRIVATE DirSizeCommon)

add_executable(debug_registration debug_registration.cpp)
target_link_libraries(debug_registration PRIVATE DirSizeCommon propsys)

add_executable(try_registration try_registration.cpp)
target_link_libraries(try_registration PRIVATE DirSizeCommon propsys)

add_executable(diag_hook diag_hook.cpp)
target_link_libraries(diag_hook PRIVATE DirSizeCommon propsys shlwapi shell32 ole32 oleaut32)
