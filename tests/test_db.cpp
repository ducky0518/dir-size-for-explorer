#include "dirsize/db.h"

#include <Windows.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

using namespace dirsize;

static std::wstring GetTempDbPath() {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"dirsize_test.db";
}

static void CleanupDb(const std::wstring& path) {
    // SQLite WAL mode creates -wal and -shm files
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + L"-wal", ec);
    std::filesystem::remove(path + L"-shm", ec);
}

void TestOpenClose() {
    std::wcout << L"TestOpenClose... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(!db.IsOpen());
    assert(db.Open(path, false));
    assert(db.IsOpen());
    db.Close();
    assert(!db.IsOpen());

    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestUpsertAndGet() {
    std::wcout << L"TestUpsertAndGet... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(db.Open(path, false));

    DirEntry entry;
    entry.path = L"C:\\Users\\Test\\Documents";
    entry.totalSize = 1234567890ULL;
    entry.fileCount = 42;
    entry.dirCount = 7;
    entry.scanTime = 1700000000;
    entry.depth = 2;

    assert(db.UpsertEntry(entry));

    // Read it back
    auto size = db.GetSize(L"C:\\Users\\Test\\Documents");
    assert(size.has_value());
    assert(*size == 1234567890ULL);

    auto fullEntry = db.GetEntry(L"C:\\Users\\Test\\Documents");
    assert(fullEntry.has_value());
    assert(fullEntry->totalSize == 1234567890ULL);
    assert(fullEntry->fileCount == 42);
    assert(fullEntry->dirCount == 7);

    db.Close();
    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestPathNormalization() {
    std::wcout << L"TestPathNormalization... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(db.Open(path, false));

    DirEntry entry;
    entry.path = L"C:\\Users\\Test\\";  // Trailing backslash
    entry.totalSize = 999;
    entry.scanTime = 1700000000;
    assert(db.UpsertEntry(entry));

    // Should find it regardless of trailing slash or case
    auto size = db.GetSize(L"C:\\Users\\Test");
    assert(size.has_value());
    assert(*size == 999);

    size = db.GetSize(L"c:\\users\\test");
    assert(size.has_value());
    assert(*size == 999);

    size = db.GetSize(L"C:/Users/Test");
    assert(size.has_value());
    assert(*size == 999);

    db.Close();
    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestBatchUpsert() {
    std::wcout << L"TestBatchUpsert... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(db.Open(path, false));

    std::vector<DirEntry> entries;
    for (int i = 0; i < 100; i++) {
        DirEntry e;
        e.path = L"C:\\Test\\Dir" + std::to_wstring(i);
        e.totalSize = i * 1000ULL;
        e.fileCount = i;
        e.dirCount = 0;
        e.scanTime = 1700000000 + i;
        e.depth = 1;
        entries.push_back(e);
    }

    assert(db.UpsertEntries(entries));

    // Verify a few
    auto size = db.GetSize(L"C:\\Test\\Dir0");
    assert(size.has_value() && *size == 0);

    size = db.GetSize(L"C:\\Test\\Dir99");
    assert(size.has_value() && *size == 99000);

    db.Close();
    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestUpdateExisting() {
    std::wcout << L"TestUpdateExisting... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(db.Open(path, false));

    DirEntry entry;
    entry.path = L"C:\\Test";
    entry.totalSize = 100;
    entry.scanTime = 1700000000;
    assert(db.UpsertEntry(entry));

    // Update with new size
    entry.totalSize = 200;
    entry.scanTime = 1700000001;
    assert(db.UpsertEntry(entry));

    auto size = db.GetSize(L"C:\\Test");
    assert(size.has_value());
    assert(*size == 200);

    db.Close();
    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestNotFound() {
    std::wcout << L"TestNotFound... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(db.Open(path, false));

    auto size = db.GetSize(L"C:\\NonExistent\\Path");
    assert(!size.has_value());

    db.Close();
    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestRemoveByPrefix() {
    std::wcout << L"TestRemoveByPrefix... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(db.Open(path, false));

    std::vector<DirEntry> entries;
    for (const auto& p : { L"C:\\A", L"C:\\A\\B", L"C:\\A\\B\\C", L"C:\\D" }) {
        DirEntry e;
        e.path = p;
        e.totalSize = 100;
        e.scanTime = 1700000000;
        entries.push_back(e);
    }
    assert(db.UpsertEntries(entries));

    // Remove C:\A and all children
    assert(db.RemoveByPrefix(L"C:\\A"));

    assert(!db.GetSize(L"C:\\A").has_value());
    assert(!db.GetSize(L"C:\\A\\B").has_value());
    assert(!db.GetSize(L"C:\\A\\B\\C").has_value());

    // C:\D should still exist
    assert(db.GetSize(L"C:\\D").has_value());

    db.Close();
    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestUsnBookmark() {
    std::wcout << L"TestUsnBookmark... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    Database db;
    assert(db.Open(path, false));

    UsnBookmark bm;
    bm.volume = L"C:";
    bm.journalId = 123456789;
    bm.lastUsn = 987654321;
    assert(db.UpsertUsnBookmark(bm));

    auto result = db.GetUsnBookmark(L"C:");
    assert(result.has_value());
    assert(result->journalId == 123456789);
    assert(result->lastUsn == 987654321);

    // Update
    bm.lastUsn = 111111111;
    assert(db.UpsertUsnBookmark(bm));

    result = db.GetUsnBookmark(L"C:");
    assert(result.has_value());
    assert(result->lastUsn == 111111111);

    db.Close();
    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

void TestReadOnlyMode() {
    std::wcout << L"TestReadOnlyMode... ";
    auto path = GetTempDbPath();
    CleanupDb(path);

    // First create the DB with some data
    {
        Database db;
        assert(db.Open(path, false));
        DirEntry entry;
        entry.path = L"C:\\Test";
        entry.totalSize = 42;
        entry.scanTime = 1700000000;
        assert(db.UpsertEntry(entry));
        db.Close();
    }

    // Open read-only
    {
        Database db;
        assert(db.Open(path, true));
        auto size = db.GetSize(L"C:\\Test");
        assert(size.has_value());
        assert(*size == 42);
        db.Close();
    }

    CleanupDb(path);
    std::wcout << L"PASSED" << std::endl;
}

int main() {
    std::wcout << L"=== DirSize Database Tests ===" << std::endl;

    TestOpenClose();
    TestUpsertAndGet();
    TestPathNormalization();
    TestBatchUpsert();
    TestUpdateExisting();
    TestNotFound();
    TestRemoveByPrefix();
    TestUsnBookmark();
    TestReadOnlyMode();

    std::wcout << L"\nAll tests passed!" << std::endl;
    return 0;
}
