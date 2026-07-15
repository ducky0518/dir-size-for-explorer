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
#include <unordered_set>
#include <vector>

namespace dirsize {

struct ScanResult {
    std::wstring path;
    uint64_t totalSize = 0;
    uint64_t allocSize = 0; // "Size on disk" — cluster-rounded
    uint64_t fileCount = 0;
    uint64_t dirCount = 0;
    // False when the enumeration failed partway (network error, shutdown)
    // anywhere in the subtree. Incomplete results must never be written to
    // the database or used for ancestor deltas — a truncated listing looks
    // exactly like a directory that shrank.
    bool complete = true;
};

// One entry in the manual-scan history (settings "Manual Scan" tab).
struct ManualScanRecord {
    std::wstring path;      // Canonical folder path
    int64_t startMs = 0;    // Unix epoch ms; 0 = still queued
    int64_t endMs = 0;      // Unix epoch ms; 0 = not finished yet
    uint64_t totalSize = 0;
    uint64_t fileCount = 0;
    uint64_t dirCount = 0;
    bool completed = false;    // false = queued/running/aborted
    bool underWatched = false; // Inside a configured watched directory?
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
    // Returns true if the path was queued, false if filtered out (not under a watched dir).
    bool QueueRescan(const std::wstring& path);

    // Reload configuration (e.g., after tray app changes settings).
    void ReloadConfig();

    // Request an immediate full scan (used by "Scan Now" tray menu item).
    void RequestFullScan();

    // Request a deep scan of one watched root, bypassing the real-time
    // suspension of interval scans. Called by a DirectoryWatcher after a
    // notification overflow or a reconnect: in both cases changes were
    // missed at unknown depths, so a shallow root rescan (which reuses
    // cached child totals) cannot repair the tree. The request is
    // persisted (Database::SetPendingScan) so it survives a restart, and
    // retried on failure.
    void RequestReconcileScan(const std::wstring& path);

    // Queue a user-requested scan of an arbitrary folder (Manual Scan
    // tab). Unlike QueueRescan this accepts paths OUTSIDE the watched
    // directories — results are written to the database so Explorer shows
    // sizes for them, letting users try the product on a folder before
    // adding it as a watched directory.
    void RequestManualScan(const std::wstring& path);

    // Snapshot of the last few manual scans, newest first.
    std::vector<ManualScanRecord> GetManualScanHistory() const;

    // A directory inside a watched tree was renamed/moved: rewrite its
    // cached subtree to the new path (preserving sizes) and queue shallow
    // rescans of both parents so ancestor totals update on both sides.
    void HandleDirectoryRename(const std::wstring& oldPath,
                               const std::wstring& newPath);

    // A watched ROOT was renamed/moved (detected via its open handle).
    // Updates the watched-dirs configuration in the registry, rewrites the
    // cached subtree, and reloads. Returns the new canonical root.
    std::wstring HandleWatchedRootRename(const std::wstring& oldCanonicalRoot,
                                         const std::wstring& newPath);

    bool IsScanning() const { return m_scanning.load(); }

    // Accessors for status reporting (used by IPC GetLog command)
    std::wstring GetCurrentPath() const;
    int64_t GetLastFullScanTime() const;

    // --- Real-time coverage tracking ---
    // A DirectoryWatcher calls MarkRealtimeActive(root) after its first
    // notification-driven rescan is queued, proving change notifications
    // work for that root. While active, interval scans for the root are
    // suspended (the watcher keeps sizes fresh). MarkRealtimeInactive is
    // called when the watcher stops, resuming interval scans.
    void MarkRealtimeActive(const std::wstring& canonicalRoot);
    void MarkRealtimeInactive(const std::wstring& canonicalRoot);
    // For the settings UI: is real-time coverage confirmed for this dir?
    bool IsRealtimeActive(const std::wstring& path);

private:
    // The scheduler thread: wakes at configured intervals or when a rescan is queued.
    void SchedulerThread();

    // Perform a full scan of all watched directories.
    void FullScan();

    // Scan a single watched root (used by FullScan and by per-root
    // interval scans). Returns true when the scan ran to completion (no
    // shutdown, no enumeration failure) — only then are stale rows purged
    // and the root's pending-scan flag cleared.
    bool ScanRoot(const std::wstring& canonicalRoot);

    // Rescan a single queued directory and propagate the size delta to
    // its ancestors (up to the nearest watched root) so parents stay fresh.
    void RescanOne(const std::wstring& path);

    // Run one queued manual scan (deep) and record it in the history.
    void ManualScanOne(const std::wstring& path);

    // Shared by RescanOne/ManualScanOne: push the old-vs-new size delta up
    // the ancestor chain (within watched roots), collecting changed dirs.
    void PropagateAncestorDeltas(const std::wstring& path,
                                 const DirEntry& oldEntry,
                                 const ScanResult& result,
                                 std::vector<std::wstring>& changedDirs);

    // Scan a single directory tree. Completed directory entries are
    // appended to outEntries and flushed to the DB in chunks.
    ScanResult ScanDirectory(const std::wstring& rootPath, int depth,
                             std::vector<DirEntry>& outEntries);

    // Flush accumulated entries to the database and clear the vector.
    void FlushEntries(std::vector<DirEntry>& entries);

    std::shared_ptr<Database> m_db;
    Config m_config;
    std::mutex m_configMutex;
    // Canonicalized copy of m_config.excludedDirs (guarded by m_configMutex)
    std::vector<std::wstring> m_canonicalExcluded;

    // True if canonicalPath equals or lies under an excluded directory.
    bool IsExcluded(const std::wstring& canonicalPath);
    void RebuildExcludedLocked(); // Caller must hold m_configMutex

    HANDLE m_stopEvent = nullptr;
    HANDLE m_rescanEvent = nullptr;  // Signaled when a rescan is queued

    std::thread m_schedulerThread;
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_fullScanRequested{false};

    // Roots owing a deep reconciliation scan (watcher overflow/reconnect),
    // mapped to the earliest tick the scan may run (retries after a failed
    // attempt are deferred instead of retried back-to-back). These scans
    // run even while real-time coverage suspends interval scans. The flag
    // is also persisted per root in the database so an engine restart
    // can't lose it.
    std::mutex m_reconcileMutex;
    std::unordered_map<std::wstring, ULONGLONG> m_reconcilePending;

    // Pending rescans: path → earliest tick it may run. A per-directory
    // cooldown coalesces the notification storms that sustained writes
    // produce (a large download re-notifies its folder on every flush):
    // the first change scans promptly, repeats within the cooldown are
    // deferred and merged into one scan.
    std::mutex m_queueMutex;
    std::unordered_map<std::wstring, ULONGLONG> m_pendingRescans;
    std::unordered_map<std::wstring, ULONGLONG> m_lastRescanDone;

    // Manual-scan queue + bounded history (session-scoped)
    mutable std::mutex m_manualMutex;
    std::queue<std::wstring> m_manualQueue;
    std::vector<ManualScanRecord> m_manualHistory; // newest first, max 5

    IOThrottle m_throttle;

    // Cluster size cache (per volume root) for allocation size computation.
    // Keys are normalized roots: "c:\\" or "\\\\server\\share\\".
    std::unordered_map<std::wstring, DWORD> m_clusterSizeCache;
    DWORD GetClusterSize(const std::wstring& path);

    // Status tracking for the Logging tab
    mutable std::mutex m_stateMutex;
    std::wstring m_currentScanPath;
    int64_t m_lastFullScanTime = 0;     // Unix epoch milliseconds

    // Roots with confirmed real-time change coverage (canonical paths).
    std::mutex m_realtimeMutex;
    std::unordered_set<std::wstring> m_realtimeRoots;

    // Per-root last interval-scan tick (scheduler thread only).
    std::unordered_map<std::wstring, ULONGLONG> m_rootLastScanTick;
};

} // namespace dirsize
