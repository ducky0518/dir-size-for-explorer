#include "dirsize/db.h"

#include <Windows.h>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace dirsize;

// This test creates a temporary directory tree, scans it using the service's
// scanner logic (replicated here for testing without the full service), and
// verifies the computed sizes match.

namespace {

std::wstring GetTempTestDir() {
    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"dirsize_scanner_test";
}

void CreateTestTree(const std::wstring& root) {
    namespace fs = std::filesystem;
    fs::create_directories(root);
    fs::create_directories(root + L"\\subdir1");
    fs::create_directories(root + L"\\subdir1\\nested");
    fs::create_directories(root + L"\\subdir2");

    // Create files with known sizes
    auto writeFile = [](const std::wstring& path, size_t size) {
        std::ofstream f(path, std::ios::binary);
        std::string data(size, 'X');
        f.write(data.data(), data.size());
    };

    writeFile(root + L"\\file1.txt", 1000);          // 1000 bytes
    writeFile(root + L"\\subdir1\\file2.txt", 2000);  // 2000 bytes
    writeFile(root + L"\\subdir1\\nested\\file3.txt", 3000);  // 3000 bytes
    writeFile(root + L"\\subdir2\\file4.txt", 4000);  // 4000 bytes
}

void CleanupTestTree(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// Simplified recursive scanner (mirrors service scanner logic)
struct ScanResult {
    uint64_t totalSize = 0;
    uint64_t fileCount = 0;
    uint64_t dirCount = 0;
};

ScanResult ScanDir(const std::wstring& path) {
    ScanResult result;

    std::wstring searchPath = path + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileExW(searchPath.c_str(), FindExInfoBasic,
                                     &fd, FindExSearchNameMatch,
                                     nullptr, FIND_FIRST_EX_LARGE_FETCH);

    if (hFind == INVALID_HANDLE_VALUE) return result;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        std::wstring childPath = path + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            ScanResult child = ScanDir(childPath);
            result.totalSize += child.totalSize;
            result.fileCount += child.fileCount;
            result.dirCount += child.dirCount + 1;
        } else {
            uint64_t fileSize =
                (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            result.totalSize += fileSize;
            result.fileCount++;
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return result;
}

} // namespace

void TestScanKnownTree() {
    std::wcout << L"TestScanKnownTree... ";

    auto testDir = GetTempTestDir();
    CleanupTestTree(testDir);
    CreateTestTree(testDir);

    ScanResult result = ScanDir(testDir);

    // Expected: file1(1000) + file2(2000) + file3(3000) + file4(4000) = 10000
    assert(result.totalSize == 10000);
    assert(result.fileCount == 4);
    assert(result.dirCount == 3); // subdir1, subdir1/nested, subdir2

    CleanupTestTree(testDir);
    std::wcout << L"PASSED" << std::endl;
}

void TestScanEmptyDir() {
    std::wcout << L"TestScanEmptyDir... ";

    auto testDir = GetTempTestDir();
    CleanupTestTree(testDir);
    std::filesystem::create_directories(testDir);

    ScanResult result = ScanDir(testDir);
    assert(result.totalSize == 0);
    assert(result.fileCount == 0);
    assert(result.dirCount == 0);

    CleanupTestTree(testDir);
    std::wcout << L"PASSED" << std::endl;
}

void TestScanNonExistent() {
    std::wcout << L"TestScanNonExistent... ";

    ScanResult result = ScanDir(L"C:\\This\\Path\\Does\\Not\\Exist\\12345");
    assert(result.totalSize == 0);
    assert(result.fileCount == 0);
    assert(result.dirCount == 0);

    std::wcout << L"PASSED" << std::endl;
}

int main() {
    std::wcout << L"=== DirSize Scanner Tests ===" << std::endl;

    TestScanKnownTree();
    TestScanEmptyDir();
    TestScanNonExistent();

    std::wcout << L"\nAll tests passed!" << std::endl;
    return 0;
}
