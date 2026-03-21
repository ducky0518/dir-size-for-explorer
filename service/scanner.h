#pragma once

#include <Windows.h>

#include "dirsize/config.h"
#include "dirsize/db.h"
#include "throttle.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dirsize {

struct ScanResult {
    std::wstring path;
    uint64_t totalSize = 0;
    uint64_t allocSize = 0; // "Size on disk" — cluster-rounded
    uint64_t fileCount = 0;
    uint64_t dirCount = 0;
};

class Scanner {
public:
    Scanner(std::shared_ptr<Database> db, const Config& config);
    ~Scanner();

    Scanner(const Scanner&) = delete;
    Scanner& operator=(const Scanner&) = delete;

    // Start the scan scheduler thread. stopEvent is signaled to shut down.
    void Start(HANDLE stopEvent);
    void Stop();

    // Queue a specific path for immediate rescan (used by IPC "Recalculate" and USN monitor).
    void QueueRescan(const std::wstring& path);

    // Reload configuration (e.g., after tray app changes settings).
    void ReloadConfig();

    bool IsScanning() const { return m_scanning.load(); }

private:
    // The scheduler thread: wakes at configured intervals or when a rescan is queued.
    void SchedulerThread();

    // Perform a full scan of all watched directories.
    void FullScan();

    // Scan a single directory tree. Returns results for all directories found.
    ScanResult ScanDirectory(const std::wstring& rootPath, int depth,
                             std::vector<DirEntry>& outEntries);

    std::shared_ptr<Database> m_db;
    Config m_config;
    std::mutex m_configMutex;

    HANDLE m_stopEvent = nullptr;
    HANDLE m_rescanEvent = nullptr;  // Signaled when a rescan is queued

    std::thread m_schedulerThread;
    std::atomic<bool> m_scanning{false};

    std::mutex m_queueMutex;
    std::queue<std::wstring> m_rescanQueue;

    IOThrottle m_throttle;

    // Cluster size cache (per drive letter) for allocation size computation
    std::unordered_map<wchar_t, DWORD> m_clusterSizeCache;
    DWORD GetClusterSize(wchar_t driveLetter);
};

} // namespace dirsize
