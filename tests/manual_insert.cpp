// Quick manual test: insert a few known directory sizes into the DB
// then you can check if the Explorer column picks them up.

#include "dirsize/db.h"
#include <iostream>
#include <chrono>
#include <filesystem>

int wmain() {
    dirsize::Database db;
    auto dbPath = dirsize::Database::GetDefaultPath();
    std::wcout << L"Opening DB at: " << dbPath << std::endl;

    if (!db.Open(dbPath, false)) {
        std::wcerr << L"Failed to open database!" << std::endl;
        return 1;
    }

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Scan and insert sizes for a few directories
    auto scanAndInsert = [&](const std::wstring& path) {
        uint64_t totalSize = 0;
        uint64_t fileCount = 0;

        try {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     path, std::filesystem::directory_options::skip_permission_denied)) {
                try {
                    if (entry.is_regular_file()) {
                        totalSize += entry.file_size();
                        fileCount++;
                    }
                } catch (...) {}
            }
        } catch (...) {}

        dirsize::DirEntry e;
        e.path = path;
        e.totalSize = totalSize;
        e.fileCount = fileCount;
        e.scanTime = now;

        if (db.UpsertEntry(e)) {
            std::wcout << L"Inserted: " << path << L" = " << totalSize
                       << L" bytes (" << fileCount << L" files)" << std::endl;
        } else {
            std::wcerr << L"Failed to insert: " << path << std::endl;
        }
    };

    // Scan some common directories the user can browse to
    scanAndInsert(L"C:\\Users\\laptop\\Desktop");
    scanAndInsert(L"C:\\Users\\laptop\\Documents");
    scanAndInsert(L"C:\\Users\\laptop\\Downloads");

    // Also scan immediate subdirectories of Desktop
    try {
        for (const auto& entry : std::filesystem::directory_iterator(L"C:\\Users\\laptop\\Desktop")) {
            if (entry.is_directory()) {
                scanAndInsert(entry.path().wstring());
            }
        }
    } catch (...) {}

    db.Close();
    std::wcout << L"\nDone! Open C:\\Users\\laptop\\Desktop in Explorer "
               << L"and enable the 'Folder Size' column." << std::endl;
    return 0;
}
