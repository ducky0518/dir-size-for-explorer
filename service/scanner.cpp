#include "scanner.h"
#include "log_buffer.h"

#include <chrono>
#include <algorithm>

namespace dirsize {

Scanner::Scanner(std::shared_ptr<Database> db, const Config& config)
    : m_db(std::move(db))
    , m_config(config)
    , m_throttle(config.ioPriority)
{
}

Scanner::~Scanner() {
    Stop();
}

void Scanner::Start(HANDLE stopEvent) {
    m_stopEvent = stopEvent;
    m_rescanEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr); // Auto-reset

    m_schedulerThread = std::thread(&Scanner::SchedulerThread, this);
}

void Scanner::Stop() {
    // The stop event is owned by the service; it's already been signaled.
    if (m_schedulerThread.joinable()) {
        // Wake the scheduler if it's sleeping
        if (m_rescanEvent) SetEvent(m_rescanEvent);
        m_schedulerThread.join();
    }
    if (m_rescanEvent) {
        CloseHandle(m_rescanEvent);
        m_rescanEvent = nullptr;
    }
}

void Scanner::QueueRescan(const std::wstring& path) {
    {
        std::lock_guard lock(m_queueMutex);
        m_rescanQueue.push(path);
    }
    Log(LogSeverity::Verbose, L"Rescan queued: %s", path.c_str());
    if (m_rescanEvent) SetEvent(m_rescanEvent);
}

void Scanner::ReloadConfig() {
    Config newConfig = LoadConfig();
    {
        std::lock_guard lock(m_configMutex);
        m_config = newConfig;
        m_throttle.SetLevel(newConfig.ioPriority);
    }
    Log(LogSeverity::Info, "Configuration reloaded");
    // Wake the scheduler so it picks up the new interval
    if (m_rescanEvent) SetEvent(m_rescanEvent);
}

std::wstring Scanner::GetCurrentPath() const {
    std::lock_guard lock(m_stateMutex);
    return m_currentScanPath;
}

int64_t Scanner::GetLastFullScanTime() const {
    std::lock_guard lock(m_stateMutex);
    return m_lastFullScanTime;
}

void Scanner::SchedulerThread() {
    m_throttle.Apply();

    // Run an initial full scan on startup
    FullScan();

    while (true) {
        uint32_t intervalMs;
        {
            std::lock_guard lock(m_configMutex);
            intervalMs = m_config.scanIntervalMinutes * 60 * 1000;
        }

        // Wait for either: stop event, rescan event, or interval timeout
        HANDLE events[] = { m_stopEvent, m_rescanEvent };
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, intervalMs);

        if (waitResult == WAIT_OBJECT_0) {
            // Stop event signaled
            break;
        }

        // Process any queued rescans
        {
            std::lock_guard lock(m_queueMutex);
            while (!m_rescanQueue.empty()) {
                std::wstring path = std::move(m_rescanQueue.front());
                m_rescanQueue.pop();

                m_scanning.store(true);
                std::vector<DirEntry> entries;
                ScanDirectory(path, 0, entries);
                if (!entries.empty()) {
                    m_db->UpsertEntries(entries);
                }
                m_scanning.store(false);
            }
        }

        // If woken by timeout (not rescan event), do a full scan
        if (waitResult == WAIT_TIMEOUT) {
            FullScan();
        }
    }
}

void Scanner::FullScan() {
    m_scanning.store(true);

    std::vector<std::wstring> watchedDirs;
    {
        std::lock_guard lock(m_configMutex);
        watchedDirs = m_config.watchedDirs;
    }

    Log(LogSeverity::Info, "Full scan started (%d directories)",
        static_cast<int>(watchedDirs.size()));
    auto fullScanStart = std::chrono::steady_clock::now();

    for (const auto& rootDir : watchedDirs) {
        if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;

        {
            std::lock_guard lock(m_stateMutex);
            m_currentScanPath = rootDir;
        }
        Log(LogSeverity::Verbose, L"Scanning %s", rootDir.c_str());
        auto dirStart = std::chrono::steady_clock::now();

        std::vector<DirEntry> entries;
        ScanResult result = ScanDirectory(rootDir, 0, entries);

        auto dirElapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - dirStart).count();
        Log(LogSeverity::Info, L"Scanned %s \u2014 %llu files, %llu dirs, %.1fs",
            rootDir.c_str(), result.fileCount, result.dirCount, dirElapsed);

        // Batch write in chunks to avoid holding the DB lock too long
        constexpr size_t kBatchSize = 500;
        for (size_t i = 0; i < entries.size(); i += kBatchSize) {
            if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;

            size_t end = (std::min)(i + kBatchSize, entries.size());
            std::vector<DirEntry> batch(entries.begin() + i, entries.begin() + end);
            m_db->UpsertEntries(batch);
        }
    }

    {
        std::lock_guard lock(m_stateMutex);
        m_currentScanPath.clear();
        auto now = std::chrono::system_clock::now();
        m_lastFullScanTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
    }

    auto fullElapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - fullScanStart).count();
    Log(LogSeverity::Info, "Full scan completed in %.1f seconds", fullElapsed);

    m_scanning.store(false);
}

ScanResult Scanner::ScanDirectory(const std::wstring& rootPath, int depth,
                                  std::vector<DirEntry>& outEntries) {
    ScanResult result;
    result.path = rootPath;

    // Build search pattern. Use \\?\ prefix for long path support.
    std::wstring searchPath = rootPath;
    if (searchPath.size() >= 2 && searchPath[1] == L':' &&
        searchPath.substr(0, 4) != L"\\\\?\\") {
        searchPath = L"\\\\?\\" + searchPath;
    }
    if (searchPath.back() != L'\\') searchPath += L'\\';
    searchPath += L'*';

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileExW(
        searchPath.c_str(),
        FindExInfoBasic,       // Don't need short names
        &findData,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH);

    if (hFind == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            Log(LogSeverity::Verbose, L"Access denied: %s", rootPath.c_str());
        }
        return result;
    }

    do {
        // Check for stop
        if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) {
            FindClose(hFind);
            return result;
        }

        // Skip . and ..
        if (wcscmp(findData.cFileName, L".") == 0 ||
            wcscmp(findData.cFileName, L"..") == 0) {
            continue;
        }

        std::wstring childPath = rootPath;
        if (childPath.back() != L'\\') childPath += L'\\';
        childPath += findData.cFileName;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Skip reparse points (junctions, symlinks) to avoid infinite loops
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }

            // Recurse into subdirectory
            ScanResult childResult = ScanDirectory(childPath, depth + 1, outEntries);
            result.totalSize += childResult.totalSize;
            result.allocSize += childResult.allocSize;
            result.fileCount += childResult.fileCount;
            result.dirCount += childResult.dirCount + 1;
        } else {
            // Regular file — accumulate size
            uint64_t fileSize =
                (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) |
                findData.nFileSizeLow;
            result.totalSize += fileSize;

            // Allocation size: round up to cluster boundary
            if (fileSize > 0 && rootPath.size() >= 2 && rootPath[1] == L':') {
                DWORD clusterSize = GetClusterSize(rootPath[0]);
                uint64_t allocFileSize =
                    ((fileSize + clusterSize - 1) / clusterSize) * clusterSize;
                result.allocSize += allocFileSize;
            }

            result.fileCount++;
        }

        m_throttle.Checkpoint();

    } while (FindNextFileW(hFind, &findData));

    FindClose(hFind);

    // Record this directory's computed size
    auto now = std::chrono::system_clock::now();
    auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();

    DirEntry entry;
    entry.path = rootPath;
    entry.totalSize = result.totalSize;
    entry.allocSize = result.allocSize;
    entry.fileCount = result.fileCount;
    entry.dirCount = result.dirCount;
    entry.scanTime = epoch;
    entry.depth = depth;
    outEntries.push_back(std::move(entry));

    return result;
}

DWORD Scanner::GetClusterSize(wchar_t driveLetter) {
    wchar_t upper = static_cast<wchar_t>(towupper(driveLetter));
    auto it = m_clusterSizeCache.find(upper);
    if (it != m_clusterSizeCache.end()) return it->second;

    wchar_t root[] = { upper, L':', L'\\', L'\0' };
    DWORD sectorsPerCluster = 0, bytesPerSector = 0;
    DWORD freeClusters = 0, totalClusters = 0;
    DWORD clusterSize = 4096; // Fallback
    if (GetDiskFreeSpaceW(root, &sectorsPerCluster, &bytesPerSector,
                          &freeClusters, &totalClusters)) {
        clusterSize = sectorsPerCluster * bytesPerSector;
    }
    m_clusterSizeCache[upper] = clusterSize;
    return clusterSize;
}

} // namespace dirsize
