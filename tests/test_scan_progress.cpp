#include "scanner.h"
#include <detours/detours.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace dirsize;

namespace {
auto realFindFirst = &FindFirstFileExW;
auto realFindNext = &FindNextFileW;
auto realSleep = &Sleep;
HANDLE gateReached = nullptr;
HANDLE gateRelease = nullptr;
std::atomic<unsigned> enumerations{0};
std::atomic<unsigned> sleepCalls{0};
std::atomic<unsigned> pauseAt{0};
std::atomic<unsigned> delayAt{0};
std::atomic<unsigned> failNextAt{0};
std::atomic<unsigned> nextCalls{0};
std::atomic<bool> countSleeps{false};

HANDLE WINAPI ObservedFindFirst(LPCWSTR name, FINDEX_INFO_LEVELS info,
    LPVOID data, FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    auto index = ++enumerations;
    if (index == delayAt.load()) realSleep(1100);
    if (index == pauseAt.load()) {
        SetEvent(gateReached);
        WaitForSingleObject(gateRelease, 10000);
    }
    return realFindFirst(name, info, data, search, filter, flags);
}

BOOL WINAPI ObservedFindNext(HANDLE handle, LPWIN32_FIND_DATAW data) {
    if (++nextCalls == failNextAt.load()) {
        failNextAt = 0;
        SetEvent(gateReached);
        SetLastError(ERROR_BAD_NET_RESP);
        return FALSE;
    }
    return realFindNext(handle, data);
}

void WINAPI ObservedSleep(DWORD ms) {
    if (countSleeps && ms > 0) ++sleepCalls;
    realSleep(ms);
}

void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct Fixture {
    fs::path base;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::shared_ptr<Database> db = std::make_shared<Database>();
    std::unique_ptr<Scanner> scanner;

    Fixture() {
        base = fs::current_path() / ("scan-test-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()));
        fs::create_directories(base / "tree");
        Require(stop != nullptr, "create stop event");
        Require(db->Open((base / "cache.db").wstring()), "open test database");
    }
    ~Fixture() {
        SetEvent(stop);
        SetEvent(gateRelease);
        scanner.reset();
        db->Close();
        CloseHandle(stop);
        // base is generated exclusively beneath this test's build directory.
        std::error_code error;
        fs::remove_all(base, error);
    }
    void Start(IOPriorityLevel level = IOPriorityLevel::Low) {
        Config config;
        config.ioPriority = level;
        config.watchedDirs = {WatchedDir{(base / "tree").wstring()}};
        scanner = std::make_unique<Scanner>(db, config);
        scanner->Start(stop);
    }
    void Finish() {
        auto deadline = GetTickCount64() + 10000;
        while (scanner->GetLastFullScanTime() == 0 && GetTickCount64() < deadline)
            realSleep(1);
        Require(scanner->GetLastFullScanTime() != 0, "scan timed out");
        SetEvent(stop);
        scanner->Stop();
    }
};

void TestVisibleBeforeRootCompletes() {
    Fixture f;
    for (int i = 0; i < 600; ++i) {
        auto child = f.base / "tree" / std::to_string(i);
        fs::create_directory(child);
        std::ofstream(child / "file.bin", std::ios::binary).put('x');
    }
    enumerations = 0;
    pauseAt = 550;
    ResetEvent(gateReached);
    ResetEvent(gateRelease);
    f.Start();
    Require(WaitForSingleObject(gateReached, 10000) == WAIT_OBJECT_0,
        "scanner never reached enumeration gate");

    Database reader;
    Require(reader.Open((f.base / "cache.db").wstring(), true), "open independent reader");
    auto count = reader.GetEntryCount();
    auto root = reader.GetEntry((f.base / "tree").wstring());
    std::cout << "Before root completion: " << count << " committed entries\n";
    reader.Close();
    Require(count >= 500, "FAIL: no results published during scan");
    Require(!root, "unfinished root was published as complete");

    // Stop at the deterministic boundary: completed child rows must survive,
    // but an incomplete root must never replace its existing cached value.
    SetEvent(f.stop);
    SetEvent(gateRelease);
    f.scanner->Stop();
    Require(f.db->GetEntryCount() >= 500, "cancel discarded completed children");
    Require(!f.db->GetEntry((f.base / "tree").wstring()), "cancel wrote partial root");
    pauseAt = 0;
}

void TestTimedFlush() {
    Fixture f;
    for (int i = 0; i < 5; ++i)
        fs::create_directory(f.base / "tree" / std::to_string(i));
    enumerations = 0;
    delayAt = 3;
    pauseAt = 4;
    ResetEvent(gateReached);
    ResetEvent(gateRelease);
    f.Start();
    Require(WaitForSingleObject(gateReached, 10000) == WAIT_OBJECT_0, "timed gate missing");
    Require(f.db->GetEntryCount() > 0, "time flush waits for 500 folders");
    SetEvent(gateRelease);
    f.Finish();
    auto entry = f.db->GetEntry((f.base / "tree").wstring());
    Require(entry && entry->totalSize == 0 && entry->dirCount == 5,
        "empty directory totals incorrect");
    std::cout << "Timed flush and empty folders: PASSED\n";
    delayAt = 0;
    pauseAt = 0;
}

void TestEnumerationFailurePreservesRoot() {
    Fixture f;
    for (int i = 0; i < 20; ++i)
        std::ofstream(f.base / "tree" / std::to_string(i)).put('x');
    DirEntry baseline;
    baseline.path = (f.base / "tree").wstring();
    baseline.totalSize = 999;
    Require(f.db->UpsertEntry(baseline), "seed cached root");
    nextCalls = 0;
    failNextAt = 5;
    ResetEvent(gateReached);
    f.Start();
    Require(WaitForSingleObject(gateReached, 10000) == WAIT_OBJECT_0, "failure not injected");
    auto deadline = GetTickCount64() + 5000;
    while (f.scanner->IsScanning() && GetTickCount64() < deadline) realSleep(1);
    Require(!f.scanner->IsScanning(), "failed scan never stopped");
    auto entry = f.db->GetEntry(baseline.path);
    Require(entry && entry->totalSize == 999, "enumeration error overwrote cached total");
    std::cout << "Enumeration failure preserves cached root: PASSED\n";
}

void TestFileHeavyTree(bool benchmark) {
    Fixture f;
    constexpr int files = 5000;
    for (int i = 0; i < files; ++i)
        std::ofstream(f.base / "tree" / std::to_string(i), std::ios::binary).put('x');
    pauseAt = 0;
    sleepCalls = 0;
    countSleeps = true;
    auto start = std::chrono::steady_clock::now();
    f.Start();
    f.Finish();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    countSleeps = false;
    auto entry = f.db->GetEntry((f.base / "tree").wstring());
    Require(entry && entry->totalSize == files && entry->fileCount == files,
        "file-heavy scan totals incorrect");
    std::cout << "5000 files: " << ms << " ms, " << sleepCalls
        << " artificial sleeps\n";
    if (!benchmark)
        Require(sleepCalls <= 1, "FAIL: directory throttle applied to every file");
}
}

int main(int argc, char** argv) {
    gateReached = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    gateRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&realFindFirst), ObservedFindFirst);
    DetourAttach(reinterpret_cast<PVOID*>(&realFindNext), ObservedFindNext);
    DetourAttach(reinterpret_cast<PVOID*>(&realSleep), ObservedSleep);
    if (DetourTransactionCommit() != NO_ERROR) return 2;
    int result = 0;
    try {
        bool benchmark = argc > 1 && std::string(argv[1]) == "--benchmark";
        if (!benchmark) {
            TestVisibleBeforeRootCompletes();
            TestTimedFlush();
            TestEnumerationFailurePreservesRoot();
        }
        TestFileHeavyTree(benchmark);
        std::cout << "PASSED\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<PVOID*>(&realFindFirst), ObservedFindFirst);
    DetourDetach(reinterpret_cast<PVOID*>(&realFindNext), ObservedFindNext);
    DetourDetach(reinterpret_cast<PVOID*>(&realSleep), ObservedSleep);
    DetourTransactionCommit();
    CloseHandle(gateReached);
    CloseHandle(gateRelease);
    return result;
}
