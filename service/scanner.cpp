#include "scanner.h"
#include "log_buffer.h"
#include "dirsize/generation.h"
#include "dirsize/path_utils.h"

#include <ShlObj.h> // SHChangeNotify

#include <chrono>
#include <algorithm>

namespace {

// Bump the shared size-data generation (invalidates the shell extension's
// in-process cache) and tell Explorer which items changed so open views
// refresh immediately.
//
// Note: paths are canonical (UNC for mapped drives). Views opened via the
// UNC path refresh instantly; views opened via a mapped letter update on
// Explorer's own refresh cadence instead, since the notification path
// doesn't match their PIDLs.
void PublishSizeChanges(const std::vector<std::wstring>& changedDirs) {
    if (changedDirs.empty()) return;
    dirsize::BumpSizeDataGeneration();
    for (const auto& dir : changedDirs) {
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, dir.c_str(), nullptr);
    }
}

} // namespace

namespace dirsize {

// Check whether `path` is equal to or a child of `dir` (case-insensitive).
static bool IsUnderDirectory(const std::wstring& path, const std::wstring& dir) {
    if (path.size() < dir.size()) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    if (path.size() == dir.size()) return true;
    if (!dir.empty() && dir.back() == L'\\') return true;
    return path[dir.size()] == L'\\';
}

Scanner::Scanner(std::shared_ptr<Database> db, const Config& config)
    : m_db(std::move(db))
    , m_config(config)
    , m_throttle(config.ioPriority)
{
    std::lock_guard lock(m_configMutex);
    RebuildExcludedLocked();
}

void Scanner::RebuildExcludedLocked() {
    m_canonicalExcluded.clear();
    for (const auto& dir : m_config.excludedDirs) {
        std::wstring canon = CanonicalizePath(dir);
        if (!canon.empty()) m_canonicalExcluded.push_back(std::move(canon));
    }
}

bool Scanner::IsExcluded(const std::wstring& canonicalPath) {
    std::lock_guard lock(m_configMutex);
    for (const auto& excl : m_canonicalExcluded) {
        if (IsUnderDirectory(canonicalPath, excl)) return true;
    }
    return false;
}

void Scanner::MarkRealtimeActive(const std::wstring& canonicalRoot) {
    {
        std::lock_guard lock(m_realtimeMutex);
        if (!m_realtimeRoots.insert(canonicalRoot).second) return;
    }
    Log(LogSeverity::Info,
        L"Change watching active for %s — interval scans suspended",
        canonicalRoot.c_str());

    // Note: an interval scan of this root that is already in flight is
    // allowed to finish. It used to be cancelled as "redundant", but a
    // scan that was running because the watcher had been DOWN is exactly
    // the scan that repairs whatever the watcher missed — cancelling it
    // on reconnect made changes from the outage permanent.
}

void Scanner::MarkRealtimeInactive(const std::wstring& canonicalRoot) {
    {
        std::lock_guard lock(m_realtimeMutex);
        if (m_realtimeRoots.erase(canonicalRoot) == 0) return;
    }
    Log(LogSeverity::Info,
        L"Real-time watching stopped for %s — interval scans resume",
        canonicalRoot.c_str());
}

bool Scanner::IsRealtimeActive(const std::wstring& path) {
    std::wstring canon = CanonicalizePath(path);
    std::lock_guard lock(m_realtimeMutex);
    return m_realtimeRoots.count(canon) != 0;
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

bool Scanner::QueueRescan(const std::wstring& path) {
    std::wstring canonicalPath = CanonicalizePath(path);

    // Only rescan paths that fall under a watched directory.
    {
        std::lock_guard lock(m_configMutex);
        bool underWatched = false;
        for (const auto& watchedDir : m_config.watchedDirs) {
            std::wstring canonicalWatchedDir = CanonicalizePath(watchedDir.path);
            if (!canonicalWatchedDir.empty() &&
                IsUnderDirectory(canonicalPath, canonicalWatchedDir)) {
                underWatched = true;
                break;
            }
        }
        if (!underWatched) {
            return false;
        }
    }
    // Never rescan excluded directories (or anything inside them)
    if (IsExcluded(canonicalPath)) {
        return false;
    }
    {
        std::lock_guard lock(m_queueMutex);

        // Per-directory cooldown: if this dir was rescanned moments ago
        // (sustained write activity re-notifies it on every flush), defer
        // the next scan instead of re-enumerating it every couple of
        // seconds. First-time changes still scan promptly.
        constexpr ULONGLONG kRescanCooldownMs = 30 * 1000;
        ULONGLONG now = GetTickCount64();
        ULONGLONG due = now;
        auto itDone = m_lastRescanDone.find(canonicalPath);
        if (itDone != m_lastRescanDone.end() &&
            now - itDone->second < kRescanCooldownMs) {
            due = itDone->second + kRescanCooldownMs;
        }

        // Dedupe: if already pending, keep the existing (earlier) due time
        if (!m_pendingRescans.try_emplace(canonicalPath, due).second) {
            return true;
        }
    }
    Log(LogSeverity::Verbose, L"Shallow rescan queued: %s", canonicalPath.c_str());
    if (m_rescanEvent) SetEvent(m_rescanEvent);
    return true;
}

void Scanner::ReloadConfig() {
    Config newConfig = LoadConfig();
    std::vector<std::wstring> oldExcluded, newExcluded;
    {
        std::lock_guard lock(m_configMutex);
        oldExcluded = m_canonicalExcluded;
        m_config = newConfig;
        m_throttle.SetLevel(newConfig.ioPriority);
        RebuildExcludedLocked();
        newExcluded = m_canonicalExcluded;
    }
    Log(LogSeverity::Info, "Configuration reloaded");

    auto contains = [](const std::vector<std::wstring>& v,
                       const std::wstring& s) {
        for (const auto& e : v) {
            if (_wcsicmp(e.c_str(), s.c_str()) == 0) return true;
        }
        return false;
    };
    auto queueParentOf = [this](const std::wstring& dir) {
        size_t sep = dir.find_last_of(L'\\');
        if (sep == std::wstring::npos || sep == 0) return;
        std::wstring parent = (sep == 2 && dir[1] == L':')
            ? dir.substr(0, 3) : dir.substr(0, sep);
        if (parent != dir) QueueRescan(parent);
    };

    // Newly excluded: purge cached entries (Explorer stops showing a size)
    // and shallow-rescan the parent — it re-sums without the excluded
    // child and propagates the delta up the ancestor chain.
    bool purgedExclusions = false;
    for (const auto& excl : newExcluded) {
        if (!contains(oldExcluded, excl)) {
            m_db->RemoveByPrefix(excl);
            queueParentOf(excl);
            purgedExclusions = true;
        }
    }

    // No-longer excluded: shallow-rescan the parent; it finds the child
    // missing from the DB and deep-scans it, restoring its sizes without
    // waiting for the next full scan.
    for (const auto& excl : oldExcluded) {
        if (!contains(newExcluded, excl)) {
            queueParentOf(excl);
        }
    }

    // Prune cached trees that are no longer under any watched root, so
    // removing a watched directory doesn't leave its rows behind forever.
    {
        std::vector<std::wstring> roots;
        {
            std::lock_guard lock(m_configMutex);
            for (const auto& dir : m_config.watchedDirs) {
                std::wstring canon = CanonicalizePath(dir.path);
                if (!canon.empty()) roots.push_back(std::move(canon));
            }
        }
        if (!roots.empty()) {
            m_db->RemoveOutsideRoots(roots);
        }
    }
    if (purgedExclusions) {
        // Excluded subtrees can be huge (NAS snapshot dirs, etc.) — give
        // the freed pages back to the filesystem.
        m_db->Vacuum();
    }
    BumpSizeDataGeneration(); // Exclusions/prunes changed visible data

    // Wake the scheduler so it picks up the new interval
    if (m_rescanEvent) SetEvent(m_rescanEvent);
}

void Scanner::RequestFullScan() {
    m_fullScanRequested.store(true);
    Log(LogSeverity::Info, "Full scan requested");
    if (m_rescanEvent) SetEvent(m_rescanEvent);
}

void Scanner::RequestReconcileScan(const std::wstring& path) {
    std::wstring canon = CanonicalizePath(path);
    if (canon.empty()) return;

    {
        std::lock_guard lock(m_reconcileMutex);
        // Already queued/retrying — keep the existing due time.
        if (!m_reconcilePending.try_emplace(canon, GetTickCount64()).second) {
            return;
        }
    }
    // Persist so a restart before the scan runs still reconciles at the
    // next startup.
    m_db->SetPendingScan(canon, true);

    Log(LogSeverity::Info,
        L"Deep reconciliation scan queued for %s (changes were missed "
        L"while notifications overflowed or the watcher was down)",
        canon.c_str());
    if (m_rescanEvent) SetEvent(m_rescanEvent);
}

void Scanner::RequestManualScan(const std::wstring& path) {
    std::wstring canonical = CanonicalizePath(path);
    if (canonical.empty()) return;

    // Is this folder actually covered by a configured watched directory?
    // (Inside a watched root AND not excluded — an excluded subfolder is
    // not maintained by the scanner even though a root spans it.)
    bool underWatched = false;
    {
        std::lock_guard lock(m_configMutex);
        for (const auto& dir : m_config.watchedDirs) {
            std::wstring root = CanonicalizePath(dir.path);
            if (!root.empty() && IsUnderDirectory(canonical, root)) {
                underWatched = true;
                break;
            }
        }
    }
    if (underWatched && IsExcluded(canonical)) {
        underWatched = false;
    }

    {
        std::lock_guard lock(m_manualMutex);
        m_manualQueue.push(canonical);

        ManualScanRecord rec;
        rec.path = canonical;
        rec.underWatched = underWatched;
        // startMs stays 0 (= queued) until the scheduler picks it up
        m_manualHistory.insert(m_manualHistory.begin(), std::move(rec));
        if (m_manualHistory.size() > 5) m_manualHistory.resize(5);
    }

    Log(LogSeverity::Info, L"Manual scan requested: %s", canonical.c_str());
    if (m_rescanEvent) SetEvent(m_rescanEvent);
}

std::vector<ManualScanRecord> Scanner::GetManualScanHistory() const {
    std::lock_guard lock(m_manualMutex);
    return m_manualHistory;
}

// Parent of a canonical path ("c:\a\b" -> "c:\a", "c:\a" -> "c:\").
// Returns empty when there is no parent (drive/share root).
static std::wstring ParentOf(const std::wstring& path) {
    size_t sep = path.find_last_of(L'\\');
    if (sep == std::wstring::npos || sep == 0) return {};
    std::wstring parent = (sep == 2 && path[1] == L':')
        ? path.substr(0, 3) : path.substr(0, sep);
    return (parent == path) ? std::wstring{} : parent;
}

void Scanner::HandleDirectoryRename(const std::wstring& oldPath,
                                    const std::wstring& newPath) {
    std::wstring oldCanon = CanonicalizePath(oldPath);
    std::wstring newCanon = CanonicalizePath(newPath);
    if (oldCanon.empty() || newCanon.empty() || oldCanon == newCanon) return;

    Log(LogSeverity::Verbose, L"Rename: %s -> %s",
        oldCanon.c_str(), newCanon.c_str());

    if (IsExcluded(newCanon)) {
        // Moved into an excluded area — drop the cached subtree.
        m_db->RemoveByPrefix(oldCanon);
    } else if (auto oldEntry = m_db->GetEntry(oldCanon)) {
        // Preserve cached sizes across the rename. depth changes when the
        // directory moved to a different tree level; derive the new depth
        // from the destination parent's entry when we have it.
        std::wstring newParent = ParentOf(newCanon);
        int depthDelta = 0;
        bool canRename = true;
        if (auto parentEntry = m_db->GetEntry(newParent)) {
            depthDelta = (parentEntry->depth + 1) - oldEntry->depth;
        } else if (newParent.empty()) {
            depthDelta = -oldEntry->depth; // Became a root-level dir
        } else {
            // Destination parent unknown — safest is to drop and rescan.
            m_db->RemoveByPrefix(oldCanon);
            canRename = false;
        }
        if (canRename) {
            m_db->RenamePrefix(oldCanon, newCanon, depthDelta);
        }
    }
    // else: nothing cached under the old name — the parent rescans below
    // will pick the directory up (deep scan) if it's new to us.

    // Fix up totals on both sides of the move.
    std::wstring oldParent = ParentOf(oldCanon);
    std::wstring newParent = ParentOf(newCanon);
    if (!oldParent.empty()) QueueRescan(oldParent);
    if (!newParent.empty() && _wcsicmp(newParent.c_str(), oldParent.c_str()) != 0) {
        QueueRescan(newParent);
    }
}

std::wstring Scanner::HandleWatchedRootRename(const std::wstring& oldCanonicalRoot,
                                              const std::wstring& newPath) {
    std::wstring newCanon = CanonicalizePath(newPath);
    if (newCanon.empty() || newCanon == oldCanonicalRoot) {
        return oldCanonicalRoot;
    }

    Log(LogSeverity::Info, L"Watched folder renamed: %s -> %s",
        oldCanonicalRoot.c_str(), newCanon.c_str());

    // Update the configuration (registry) so the change survives restarts.
    Config updated;
    {
        std::lock_guard lock(m_configMutex);
        for (auto& dir : m_config.watchedDirs) {
            if (CanonicalizePath(dir.path) == oldCanonicalRoot) {
                dir.path = newPath;
            }
        }
        RebuildExcludedLocked();
        updated = m_config;
    }
    if (!SaveConfig(updated)) {
        Log(LogSeverity::Error,
            "Failed to save renamed watched folder to registry");
    }

    // Carry the cached sizes over to the new path (root keeps depth 0).
    m_db->RenamePrefix(oldCanonicalRoot, newCanon, 0);

    // Carry the real-time coverage flag over as well.
    {
        std::lock_guard lock(m_realtimeMutex);
        if (m_realtimeRoots.erase(oldCanonicalRoot) > 0) {
            m_realtimeRoots.insert(newCanon);
        }
    }

    return newCanon;
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

    // Prune trees that stopped being watched while the engine was off
    {
        std::vector<std::wstring> roots;
        {
            std::lock_guard lock(m_configMutex);
            for (const auto& dir : m_config.watchedDirs) {
                std::wstring canon = CanonicalizePath(dir.path);
                if (!canon.empty()) roots.push_back(std::move(canon));
            }
        }
        if (!roots.empty()) {
            m_db->RemoveOutsideRoots(roots);
        }
    }

    // Run an initial full scan on startup
    FullScan();
    ULONGLONG lastFullScanTick = GetTickCount64();

    auto writeHeartbeat = [this] {
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        m_db->SetMetaInt64("last_live", nowMs);
    };

    while (true) {
        // Liveness heartbeat: while the engine runs with watchers armed,
        // cached data is at most a minute staler than this timestamp. At
        // the next startup it bounds the "blind window" and decides
        // whether a reconciliation scan is needed. The loop wakes at
        // least every 60 s (waitMs is capped), so this stays current.
        writeHeartbeat();
        // Deadline-based scheduling, per watched root: each root's next
        // interval scan is due a fixed interval after ITS last scan.
        // Roots with confirmed real-time coverage are skipped entirely —
        // their watcher keeps them fresh, so re-check only once a minute
        // in case coverage is lost.
        DWORD waitMs = 60000;
        {
            std::lock_guard lock(m_configMutex);
            ULONGLONG now = GetTickCount64();
            for (const auto& dir : m_config.watchedDirs) {
                std::wstring canon = CanonicalizePath(dir.path);
                if (canon.empty()) continue;

                auto it = m_rootLastScanTick.find(canon);
                bool baselineDone = (it != m_rootLastScanTick.end());

                // Real-time coverage only suspends interval scans for
                // roots that already have a completed baseline scan. A
                // newly added directory whose watcher fires before its
                // first scan must still get that scan — otherwise its
                // sizes would never be populated at all.
                if (baselineDone) {
                    std::lock_guard rt(m_realtimeMutex);
                    if (m_realtimeRoots.count(canon)) continue;
                }

                ULONGLONG last = baselineDone ? it->second : lastFullScanTick;
                ULONGLONG due = last +
                    static_cast<ULONGLONG>(dir.scanIntervalMinutes) * 60 * 1000;
                if (!baselineDone) due = now; // New root: scan ASAP
                ULONGLONG remaining = (due > now) ? (due - now) : 0;
                if (remaining < waitMs) waitMs = static_cast<DWORD>(remaining);
            }
        }

        // Also wake in time for cooldown-deferred rescans
        {
            std::lock_guard lock(m_queueMutex);
            ULONGLONG now = GetTickCount64();
            for (const auto& pending : m_pendingRescans) {
                ULONGLONG remaining = (pending.second > now)
                    ? (pending.second - now) : 0;
                if (remaining < waitMs) waitMs = static_cast<DWORD>(remaining);
            }
        }

        // ... and for queued/retrying reconciliation scans
        {
            std::lock_guard lock(m_reconcileMutex);
            ULONGLONG now = GetTickCount64();
            for (const auto& pending : m_reconcilePending) {
                ULONGLONG remaining = (pending.second > now)
                    ? (pending.second - now) : 0;
                if (remaining < waitMs) waitMs = static_cast<DWORD>(remaining);
            }
        }

        // Wait for either: stop event, rescan event, or a scan deadline
        HANDLE events[] = { m_stopEvent, m_rescanEvent };
        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, waitMs);

        if (waitResult == WAIT_OBJECT_0) {
            // Stop event signaled — record liveness up to this moment so
            // the next startup measures the blind window from shutdown,
            // not from the last periodic heartbeat.
            writeHeartbeat();
            break;
        }

        // Manual scans first — a user is actively waiting on these.
        for (;;) {
            if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;

            std::wstring manualPath;
            {
                std::lock_guard lock(m_manualMutex);
                if (m_manualQueue.empty()) break;
                manualPath = std::move(m_manualQueue.front());
                m_manualQueue.pop();
            }
            ManualScanOne(manualPath);
        }

        // Process due rescans (incremental — don't set m_scanning so the
        // tray icon only animates during full scans, not brief
        // single-directory rescans from change notifications).
        //
        // IMPORTANT: pick under the lock, but scan with the lock RELEASED —
        // otherwise the watcher and IPC threads block in QueueRescan for
        // the duration of the scan.
        for (;;) {
            if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;

            constexpr ULONGLONG kRescanCooldownMs = 30 * 1000;
            std::wstring path;
            {
                std::lock_guard lock(m_queueMutex);
                ULONGLONG now = GetTickCount64();
                for (auto it = m_pendingRescans.begin();
                     it != m_pendingRescans.end(); ++it) {
                    if (it->second > now) continue; // Not due yet

                    // Re-check the cooldown at pick time: the dir may have
                    // been rescanned after this entry was queued.
                    auto itDone = m_lastRescanDone.find(it->first);
                    if (itDone != m_lastRescanDone.end() &&
                        now - itDone->second < kRescanCooldownMs) {
                        it->second = itDone->second + kRescanCooldownMs;
                        continue;
                    }

                    path = it->first;
                    m_pendingRescans.erase(it);
                    break;
                }
            }
            if (path.empty()) break; // Nothing due right now

            RescanOne(path);

            {
                std::lock_guard lock(m_queueMutex);
                m_lastRescanDone[path] = GetTickCount64();
                // Bounded memory: drop cold entries once the map grows
                if (m_lastRescanDone.size() > 4096) {
                    ULONGLONG cutoff = GetTickCount64() - 10 * kRescanCooldownMs;
                    for (auto it = m_lastRescanDone.begin();
                         it != m_lastRescanDone.end();) {
                        it = (it->second < cutoff) ? m_lastRescanDone.erase(it)
                                                   : std::next(it);
                    }
                }
            }
        }

        // Snapshot per-root scheduling info from the current config
        struct RootSchedule {
            std::wstring canonical;
            ULONGLONG intervalMs;
        };
        std::vector<RootSchedule> roots;
        {
            std::lock_guard lock(m_configMutex);
            for (const auto& dir : m_config.watchedDirs) {
                std::wstring canon = CanonicalizePath(dir.path);
                if (canon.empty()) continue;
                roots.push_back({ std::move(canon),
                    static_cast<ULONGLONG>(dir.scanIntervalMinutes) * 60 * 1000 });
            }
        }

        bool forceAll = m_fullScanRequested.exchange(false);
        ULONGLONG now = GetTickCount64();

        // Retry cadence for reconciliation scans that fail (e.g. the share
        // dropped again mid-scan) — deferred instead of retried in a loop.
        constexpr ULONGLONG kReconcileRetryMs = 15 * 60 * 1000;

        for (const auto& root : roots) {
            if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;

            auto it = m_rootLastScanTick.find(root.canonical);
            bool baselineDone = (it != m_rootLastScanTick.end());

            // A due reconciliation scan (watcher overflow/reconnect)
            // overrides the real-time suspension: it exists precisely
            // because the watcher missed changes.
            bool reconcileDue = false;
            {
                std::lock_guard lock(m_reconcileMutex);
                auto rit = m_reconcilePending.find(root.canonical);
                reconcileDue = (rit != m_reconcilePending.end() &&
                                rit->second <= now);
            }

            // Interval scans are suspended for roots with confirmed
            // real-time coverage (the watcher keeps them fresh) — but
            // only once the root has its baseline scan; a newly added
            // directory must be scanned once no matter what. "Scan Now"
            // and reconciliation scans override everything.
            if (!forceAll && !reconcileDue && baselineDone &&
                IsRealtimeActive(root.canonical)) {
                continue;
            }

            ULONGLONG last = baselineDone ? it->second : lastFullScanTick;
            if (forceAll || reconcileDue || !baselineDone ||
                now >= last + root.intervalMs) {
                // Info-level so long-running scans are visible in the log
                // (previously only the Verbose "Scanning" line existed,
                // making a busy status inexplicable at Normal verbosity).
                // Label each scan by what triggered it: Scan Now, a
                // reconciliation, a missing baseline, or the interval
                // schedule.
                Log(LogSeverity::Info, L"%s scan started: %s",
                    forceAll ? L"Full (Scan Now)"
                             : (reconcileDue
                                    ? L"Reconciliation"
                                    : (baselineDone ? L"Interval"
                                                    : L"Baseline")),
                    root.canonical.c_str());
                m_scanning.store(true);
                m_throttle.Apply();
                bool completed = ScanRoot(root.canonical);
                m_scanning.store(false);
                m_rootLastScanTick[root.canonical] = GetTickCount64();

                std::lock_guard lock(m_reconcileMutex);
                if (completed) {
                    // Any fully completed deep scan of the root settles an
                    // outstanding reconciliation, whatever triggered it.
                    m_reconcilePending.erase(root.canonical);
                } else if (m_reconcilePending.count(root.canonical)) {
                    m_reconcilePending[root.canonical] =
                        GetTickCount64() + kReconcileRetryMs;
                }
            }
        }
    }
}

void Scanner::FullScan() {
    m_scanning.store(true);

    // Re-apply thread priority in case the IO priority setting changed
    // since the last scan (ReloadConfig only stores the new level).
    m_throttle.Apply();

    struct RootInfo {
        std::wstring canonical;
        int64_t intervalMs;
    };
    std::vector<RootInfo> roots;
    {
        std::lock_guard lock(m_configMutex);
        for (const auto& dir : m_config.watchedDirs) {
            std::wstring canon = CanonicalizePath(dir.path);
            roots.push_back({ canon.empty() ? dir.path : canon,
                static_cast<int64_t>(dir.scanIntervalMinutes) * 60 * 1000 });
        }
    }

    // Change notifications only exist while the engine runs — anything
    // that happened on the shares while we were off is invisible. The
    // scheduler writes a liveness heartbeat every minute; data can't be
    // staler than the last heartbeat. If the blind window exceeds a
    // root's interval setting, reconcile with a full scan of that root;
    // otherwise reuse the baseline (quick restarts cost nothing).
    int64_t heartbeatMs = m_db->GetMetaInt64("last_live", 0);
    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    Log(LogSeverity::Info, "Startup check: %d watched directories",
        static_cast<int>(roots.size()));
    auto fullScanStart = std::chrono::steady_clock::now();

    for (const auto& root : roots) {
        if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) break;

        // A scan of this root started but never completed (shutdown or
        // enumeration failure mid-scan), or a watcher requested a
        // reconciliation that never ran. The heartbeat can't vouch for
        // this root — scan it regardless of apparent freshness.
        if (m_db->GetPendingScan(root.canonical)) {
            Log(LogSeverity::Info,
                L"Resuming interrupted/owed scan from previous session: %s",
                root.canonical.c_str());
            ScanRoot(root.canonical);
            m_rootLastScanTick[root.canonical] = GetTickCount64();
            continue;
        }

        if (auto entry = m_db->GetEntry(root.canonical)) {
            int64_t freshAsOfMs = entry->scanTime * 1000; // epoch s -> ms
            if (heartbeatMs > freshAsOfMs) freshAsOfMs = heartbeatMs;
            int64_t staleMs = nowMs - freshAsOfMs;

            if (staleMs > root.intervalMs) {
                Log(LogSeverity::Info,
                    L"Reconciliation scan (engine was off ~%lld min): %s",
                    staleMs / 60000, root.canonical.c_str());
                ScanRoot(root.canonical);
                m_rootLastScanTick[root.canonical] = GetTickCount64();
                continue;
            }

            Log(LogSeverity::Info,
                L"Using existing baseline for %s (fresh within interval)",
                root.canonical.c_str());
            m_rootLastScanTick[root.canonical] = GetTickCount64();

            // Reflect the baseline's age in the status line ("Last scan")
            // instead of showing "Never" when nothing ran this session.
            {
                std::lock_guard lock(m_stateMutex);
                if (freshAsOfMs > m_lastFullScanTime) {
                    m_lastFullScanTime = freshAsOfMs;
                }
            }
            continue;
        }

        ScanRoot(root.canonical);
        m_rootLastScanTick[root.canonical] = GetTickCount64();
    }

    auto fullElapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - fullScanStart).count();
    Log(LogSeverity::Info, "Startup check completed in %.1f seconds", fullElapsed);

    m_scanning.store(false);
}

bool Scanner::ScanRoot(const std::wstring& canonicalRoot) {
    {
        std::lock_guard lock(m_stateMutex);
        m_currentScanPath = canonicalRoot;
    }
    Log(LogSeverity::Verbose, L"Scanning %s", canonicalRoot.c_str());
    auto dirStart = std::chrono::steady_clock::now();

    // Mark the scan as owed until it completes. If it's interrupted
    // (shutdown, network failure), the flag survives in the database and
    // forces a reconciliation at the next startup \u2014 the liveness
    // heartbeat alone must not make a half-scanned tree look fresh.
    m_db->SetPendingScan(canonicalRoot, true);
    int64_t scanStartEpoch = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<DirEntry> entries;
    ScanResult result = ScanDirectory(canonicalRoot, 0, entries);

    auto dirElapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - dirStart).count();
    Log(LogSeverity::Info, L"Scanned %s \u2014 %llu files, %llu dirs, %.1fs",
        canonicalRoot.c_str(), result.fileCount, result.dirCount, dirElapsed);

    bool completed = result.complete &&
        WaitForSingleObject(m_stopEvent, 0) != WAIT_OBJECT_0;

    // ScanDirectory flushes in chunks as it goes; write the remainder.
    if (completed) {
        FlushEntries(entries);

        // Every directory that still exists was just upserted with a
        // fresh scan_time; anything under the root that predates this
        // scan was deleted from disk while we weren't looking. Purge it \u2014
        // full scans previously only ever added rows, so subtrees deleted
        // during downtime lingered in the cache forever.
        m_db->RemoveStaleUnder(canonicalRoot, scanStartEpoch);

        // The scan (and the stale purge) ran to completion \u2014 the root is
        // reconciled and no longer owed a scan.
        m_db->SetPendingScan(canonicalRoot, false);

        // Fresh data for the whole tree: invalidate the shell cache and
        // refresh any open view of the root.
        BumpSizeDataGeneration();
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW,
                       canonicalRoot.c_str(), nullptr);
    } else if (!result.complete) {
        Log(LogSeverity::Error,
            L"Scan of %s did not complete (enumeration failure) \u2014 cached "
            L"data kept as-is; the scan will be retried",
            canonicalRoot.c_str());
    }

    {
        std::lock_guard lock(m_stateMutex);
        m_currentScanPath.clear();
        auto nowSys = std::chrono::system_clock::now();
        m_lastFullScanTime = std::chrono::duration_cast<std::chrono::milliseconds>(
            nowSys.time_since_epoch()).count();
    }
    return completed;
}

// Rescan a single directory (triggered by a change notification or
// "Recalculate Size") and propagate the size delta up the ancestor chain
// so parent folders stay accurate without waiting for the next full scan.
//
// This is a SHALLOW rescan: only the directory's own level is enumerated.
// Subdirectory totals are reused from the database, because changes deeper
// in the tree raise their own notifications for their own parent
// directories. That makes an incremental update O(one directory listing)
// instead of O(entire subtree) — a large win on high-latency filesystems
// like SMB shares. Subdirectories not yet in the database (newly created)
// are deep-scanned once as a fallback.
void Scanner::RescanOne(const std::wstring& path) {
    auto rescanStart = std::chrono::steady_clock::now();

    // Snapshot the old entry before rescanning so we can compute the delta.
    auto oldEntry = m_db->GetEntry(path);
    int depth = oldEntry ? oldEntry->depth : 0;

    ScanResult result;
    result.path = path;

    std::wstring searchPath = path;
    if (searchPath.size() >= 2 && searchPath[1] == L':' &&
        searchPath.substr(0, 4) != L"\\\\?\\") {
        searchPath = L"\\\\?\\" + searchPath;
    } else if (searchPath.size() >= 2 &&
               searchPath[0] == L'\\' && searchPath[1] == L'\\' &&
               searchPath.substr(0, 4) != L"\\\\?\\") {
        searchPath = L"\\\\?\\UNC\\" + searchPath.substr(2);
    }
    if (searchPath.empty()) return;
    if (searchPath.back() != L'\\') searchPath += L'\\';
    searchPath += L'*';

    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileExW(searchPath.c_str(), FindExInfoBasic,
                                    &findData, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (hFind == INVALID_HANDLE_VALUE) {
        // Directory gone (deleted/renamed) — drop it and its subtree.
        if (GetLastError() == ERROR_FILE_NOT_FOUND ||
            GetLastError() == ERROR_PATH_NOT_FOUND) {
            m_db->RemoveByPrefix(path);
            Log(LogSeverity::Verbose,
                L"Shallow rescan: %s no longer exists — purged", path.c_str());

            // Roll the loss up to ancestors. Without this, a directory
            // removed as a whole (rather than emptied in place) leaves
            // every ancestor's cached total overcounted by its old size
            // until the next full/interval scan — the parent may never
            // get its own change notification if nothing changed at its
            // own level (only deeper in the tree), so this is the only
            // place that delta gets applied.
            if (oldEntry) {
                ScanResult zeroResult;
                zeroResult.path = path;
                std::vector<std::wstring> changedDirs;
                changedDirs.push_back(path);
                PropagateAncestorDeltas(path, *oldEntry, zeroResult, changedDirs);
                PublishSizeChanges(changedDirs);
            }
        }
        return;
    }

    std::vector<std::wstring> liveChildDirs;
    bool aborted = false;
    bool childScanFailed = false;

    do {
        if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) {
            aborted = true;
            break;
        }
        if (wcscmp(findData.cFileName, L".") == 0 ||
            wcscmp(findData.cFileName, L"..") == 0) {
            continue;
        }

        std::wstring childPath = path;
        if (childPath.back() != L'\\') childPath += L'\\';
        childPath += findData.cFileName;

        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }
            std::wstring canonicalChild = CanonicalizePath(childPath);

            // Excluded directories: not counted, not scanned, and not
            // added to liveChildDirs so any stale DB subtree gets purged.
            if (IsExcluded(canonicalChild)) {
                continue;
            }
            liveChildDirs.push_back(canonicalChild);

            if (auto childEntry = m_db->GetEntry(canonicalChild)) {
                // Reuse stored totals — deeper changes notify separately.
                result.totalSize += childEntry->totalSize;
                result.allocSize += childEntry->allocSize;
                result.fileCount += childEntry->fileCount;
                result.dirCount += childEntry->dirCount + 1;
            } else {
                // New subdirectory — deep scan it once.
                std::vector<DirEntry> entries;
                ScanResult childResult =
                    ScanDirectory(canonicalChild, depth + 1, entries);
                if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) {
                    aborted = true;
                    break;
                }
                if (!childResult.complete) {
                    // The child's totals are truncated — folding them into
                    // this directory would write a wrong entry and
                    // propagate a wrong delta to every ancestor.
                    childScanFailed = true;
                    break;
                }
                FlushEntries(entries);
                result.totalSize += childResult.totalSize;
                result.allocSize += childResult.allocSize;
                result.fileCount += childResult.fileCount;
                result.dirCount += childResult.dirCount + 1;
            }
        } else {
            uint64_t fileSize =
                (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) |
                findData.nFileSizeLow;
            result.totalSize += fileSize;
            if (fileSize > 0) {
                DWORD clusterSize = GetClusterSize(path);
                if (clusterSize > 0) {
                    result.allocSize +=
                        ((fileSize + clusterSize - 1) / clusterSize) * clusterSize;
                } else {
                    result.allocSize += fileSize;
                }
            }
            result.fileCount++;
        }
    } while (FindNextFileW(hFind, &findData));

    // Capture the enumeration outcome BEFORE FindClose can overwrite it.
    // Only meaningful when the loop ended because FindNextFileW returned
    // FALSE (i.e. not via one of the break paths above).
    DWORD enumErr = GetLastError();

    FindClose(hFind);

    // If shutting down, the enumeration may be partial — don't write
    // incomplete numbers or propagate a bogus delta.
    if (aborted || WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) return;

    // A listing that ends with anything other than ERROR_NO_MORE_FILES
    // (SMB session drop, device error) is TRUNCATED, not complete.
    // Treating it as complete would both write undercounted totals and —
    // worse — purge every cached child that happened to come after the
    // failure point. Abandon this rescan and retry after the cooldown.
    if (childScanFailed || enumErr != ERROR_NO_MORE_FILES) {
        Log(LogSeverity::Error,
            L"Shallow rescan of %s failed mid-enumeration (error %lu) — "
            L"keeping cached data, retrying",
            path.c_str(), childScanFailed ? 0 : enumErr);
        QueueRescan(path);
        return;
    }

    // Purge database subtrees for child directories that no longer exist.
    for (const auto& dbChild : m_db->GetChildDirs(path, depth)) {
        bool stillExists = false;
        for (const auto& live : liveChildDirs) {
            if (_wcsicmp(dbChild.c_str(), live.c_str()) == 0) {
                stillExists = true;
                break;
            }
        }
        if (!stillExists) {
            m_db->RemoveByPrefix(dbChild);
        }
    }

    // Write this directory's fresh entry.
    {
        auto now = std::chrono::system_clock::now();
        DirEntry entry;
        entry.path = path;
        entry.totalSize = result.totalSize;
        entry.allocSize = result.allocSize;
        entry.fileCount = result.fileCount;
        entry.dirCount = result.dirCount;
        entry.scanTime = std::chrono::duration_cast<std::chrono::seconds>(
            now.time_since_epoch()).count();
        entry.depth = depth;
        m_db->UpsertEntry(entry);
    }

    // Track everything whose displayed size changed, so Explorer views
    // can be refreshed once the DB writes are done.
    std::vector<std::wstring> changedDirs;
    changedDirs.push_back(path);

    // If we had a previous value, apply the delta to every ancestor up to
    // (and including) the nearest watched root.
    if (oldEntry) {
        PropagateAncestorDeltas(path, *oldEntry, result, changedDirs);
    } else {
        // Newly-known directory (e.g. a folder moved in while the engine
        // wasn't running): there's no baseline to diff, so ancestors can't
        // be delta-updated from here — without this, the new directory's
        // total silently never rolled up into its parents. Queue a rescan
        // of the parent instead: it re-sums its children from a fresh
        // listing (picking up this one's totals from the DB), and since
        // the parent has a baseline, ITS delta propagates upward normally.
        std::wstring parent = ParentOf(path);
        if (!parent.empty()) {
            QueueRescan(parent);
        }
    }
    PublishSizeChanges(changedDirs);

    // Close the loop on the "Rescan queued" log line — without this,
    // queued rescans looked like they vanished into an idle engine.
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - rescanStart).count();
    Log(LogSeverity::Verbose, L"Shallow rescan done: %s — %llu files, %lld ms",
        path.c_str(), result.fileCount, static_cast<long long>(elapsedMs));
}

void Scanner::PropagateAncestorDeltas(const std::wstring& path,
                                      const DirEntry& oldEntry,
                                      const ScanResult& result,
                                      std::vector<std::wstring>& changedDirs) {
    int64_t dTotal = static_cast<int64_t>(result.totalSize) -
                     static_cast<int64_t>(oldEntry.totalSize);
    int64_t dAlloc = static_cast<int64_t>(result.allocSize) -
                     static_cast<int64_t>(oldEntry.allocSize);
    int64_t dFiles = static_cast<int64_t>(result.fileCount) -
                     static_cast<int64_t>(oldEntry.fileCount);
    int64_t dDirs  = static_cast<int64_t>(result.dirCount) -
                     static_cast<int64_t>(oldEntry.dirCount);
    if (dTotal == 0 && dAlloc == 0 && dFiles == 0 && dDirs == 0) return;

    std::vector<std::wstring> watchedRoots;
    {
        std::lock_guard lock(m_configMutex);
        for (const auto& dir : m_config.watchedDirs) {
            std::wstring canon = CanonicalizePath(dir.path);
            if (!canon.empty()) watchedRoots.push_back(std::move(canon));
        }
    }

    auto applyDelta = [](uint64_t base, int64_t delta) -> uint64_t {
        int64_t v = static_cast<int64_t>(base) + delta;
        return v > 0 ? static_cast<uint64_t>(v) : 0;
    };

    std::wstring current = path;
    for (;;) {
        size_t sep = current.find_last_of(L'\\');
        if (sep == std::wstring::npos || sep == 0) break;
        // Keep the backslash for drive roots ("c:\"), drop it otherwise
        std::wstring parent = (sep == 2 && current[1] == L':')
            ? current.substr(0, 3) : current.substr(0, sep);
        if (parent == current) break;

        bool underWatched = false;
        for (const auto& root : watchedRoots) {
            if (IsUnderDirectory(parent, root)) { underWatched = true; break; }
        }
        if (!underWatched) break;

        if (auto parentEntry = m_db->GetEntry(parent)) {
            parentEntry->totalSize = applyDelta(parentEntry->totalSize, dTotal);
            parentEntry->allocSize = applyDelta(parentEntry->allocSize, dAlloc);
            parentEntry->fileCount = applyDelta(parentEntry->fileCount, dFiles);
            parentEntry->dirCount  = applyDelta(parentEntry->dirCount, dDirs);
            m_db->UpsertEntry(*parentEntry);
            changedDirs.push_back(parent);
        }
        current = std::move(parent);
    }
}

void Scanner::ManualScanOne(const std::wstring& path) {
    auto epochMs = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };

    // Mark the newest matching queued record as started
    {
        std::lock_guard lock(m_manualMutex);
        for (auto& rec : m_manualHistory) {
            if (rec.path == path && rec.endMs == 0 && rec.startMs == 0) {
                rec.startMs = epochMs();
                break;
            }
        }
    }

    Log(LogSeverity::Info, L"Manual scan started: %s", path.c_str());
    m_scanning.store(true);
    m_throttle.Apply();
    {
        std::lock_guard lock(m_stateMutex);
        m_currentScanPath = path;
    }

    // Depth: keep it consistent with existing rows if we have them, or
    // hang it off the parent's depth; standalone folders start at 0.
    auto oldEntry = m_db->GetEntry(path);
    int depth = 0;
    if (oldEntry) {
        depth = oldEntry->depth;
    } else {
        size_t sep = path.find_last_of(L'\\');
        if (sep != std::wstring::npos && sep > 0) {
            std::wstring parent = (sep == 2 && path[1] == L':')
                ? path.substr(0, 3) : path.substr(0, sep);
            if (auto parentEntry = m_db->GetEntry(parent)) {
                depth = parentEntry->depth + 1;
            }
        }
    }

    std::vector<DirEntry> entries;
    ScanResult result = ScanDirectory(path, depth, entries);

    bool aborted = WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0 ||
                   !result.complete;
    if (!aborted) {
        FlushEntries(entries);

        std::vector<std::wstring> changedDirs;
        changedDirs.push_back(path);
        if (oldEntry) {
            PropagateAncestorDeltas(path, *oldEntry, result, changedDirs);
        }
        PublishSizeChanges(changedDirs);

        Log(LogSeverity::Info,
            L"Manual scan completed: %s — %llu files, %llu dirs",
            path.c_str(), result.fileCount, result.dirCount);
    } else if (!result.complete) {
        Log(LogSeverity::Error,
            L"Manual scan of %s failed mid-enumeration — results discarded",
            path.c_str());
    }

    // Record the outcome
    {
        std::lock_guard lock(m_manualMutex);
        for (auto& rec : m_manualHistory) {
            if (rec.path == path && rec.endMs == 0 && rec.startMs != 0) {
                rec.endMs = epochMs();
                rec.totalSize = result.totalSize;
                rec.fileCount = result.fileCount;
                rec.dirCount = result.dirCount;
                rec.completed = !aborted;
                break;
            }
        }
    }

    {
        std::lock_guard lock(m_stateMutex);
        m_currentScanPath.clear();
    }
    m_scanning.store(false);
}

ScanResult Scanner::ScanDirectory(const std::wstring& rootPath, int depth,
                                  std::vector<DirEntry>& outEntries) {
    ScanResult result;
    result.path = rootPath;

    // Build search pattern. Use \\?\ prefix for long path support
    // (drive-letter and UNC paths alike).
    std::wstring searchPath = rootPath;
    if (searchPath.size() >= 2 && searchPath[1] == L':' &&
        searchPath.substr(0, 4) != L"\\\\?\\") {
        searchPath = L"\\\\?\\" + searchPath;
    } else if (searchPath.size() >= 2 &&
               searchPath[0] == L'\\' && searchPath[1] == L'\\' &&
               searchPath.substr(0, 4) != L"\\\\?\\") {
        searchPath = L"\\\\?\\UNC\\" + searchPath.substr(2);
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
            // Permanent and expected (system dirs, other users' folders):
            // contributes zero, same as every du-style tool. Not counted
            // as a failure — retrying would never succeed.
            Log(LogSeverity::Verbose, L"Access denied: %s", rootPath.c_str());
        } else if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            // Deleted between listing and scanning — zero is correct.
        } else {
            // Transient failure (network drop, device error): the subtree
            // could not be measured at all. Mark the result incomplete so
            // no caller writes it or folds it into a parent as zero.
            result.complete = false;
            Log(LogSeverity::Error,
                L"Failed to scan %s (FindFirstFileExW error %lu)",
                rootPath.c_str(), err);
        }
        return result;
    }

    do {
        // Check for stop — a partial listing must not be recorded.
        if (WaitForSingleObject(m_stopEvent, 0) == WAIT_OBJECT_0) {
            FindClose(hFind);
            result.complete = false;
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

            // Skip excluded directories entirely (childPath inherits the
            // canonical root, so the case-insensitive prefix check works)
            if (IsExcluded(childPath)) {
                continue;
            }

            // Recurse into subdirectory
            ScanResult childResult = ScanDirectory(childPath, depth + 1, outEntries);
            result.totalSize += childResult.totalSize;
            result.allocSize += childResult.allocSize;
            result.fileCount += childResult.fileCount;
            result.dirCount += childResult.dirCount + 1;
            if (!childResult.complete) {
                // A truncated child makes this directory's totals wrong
                // too. Keep enumerating (siblings still get fresh rows)
                // but don't record an entry for this directory.
                result.complete = false;
            }
        } else {
            // Regular file — accumulate size
            uint64_t fileSize =
                (static_cast<uint64_t>(findData.nFileSizeHigh) << 32) |
                findData.nFileSizeLow;
            result.totalSize += fileSize;

            // Allocation size: round up to cluster boundary.
            // If cluster size can't be determined, fall back to logical size.
            if (fileSize > 0) {
                DWORD clusterSize = GetClusterSize(rootPath);
                if (clusterSize > 0) {
                    uint64_t allocFileSize =
                        ((fileSize + clusterSize - 1) / clusterSize) * clusterSize;
                    result.allocSize += allocFileSize;
                } else {
                    result.allocSize += fileSize;
                }
            }

            result.fileCount++;
        }

    } while (FindNextFileW(hFind, &findData));

    // Capture the outcome before FindClose can overwrite the last error.
    DWORD enumErr = GetLastError();

    FindClose(hFind);

    // Ending on anything but ERROR_NO_MORE_FILES means the listing was
    // cut short (SMB/network failure) — the totals are undercounts.
    if (enumErr != ERROR_NO_MORE_FILES) {
        result.complete = false;
        Log(LogSeverity::Error,
            L"Enumeration of %s failed partway (error %lu) — subtree "
            L"totals discarded", rootPath.c_str(), enumErr);
    }

    // Throttle once per directory (not per file) — per-file sleeps made
    // large scans take orders of magnitude longer than intended.
    m_throttle.Checkpoint();

    // Record this directory's computed size — but never an incomplete
    // one: a truncated total is indistinguishable from a directory that
    // shrank, and it would poison ancestor deltas and stale-row purges.
    if (!result.complete) {
        return result;
    }

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

    // Flush periodically so a full scan of a large volume doesn't buffer
    // millions of entries (hundreds of MB) in memory before writing.
    constexpr size_t kFlushThreshold = 4096;
    if (outEntries.size() >= kFlushThreshold) {
        FlushEntries(outEntries);
    }

    return result;
}

void Scanner::FlushEntries(std::vector<DirEntry>& entries) {
    if (entries.empty()) return;
    m_db->UpsertEntries(entries);
    entries.clear();
}

DWORD Scanner::GetClusterSize(const std::wstring& path) {
    std::wstring root = GetVolumeRootPath(path);
    if (root.empty()) return 0;

    std::wstring cacheKey = root;
    std::transform(cacheKey.begin(), cacheKey.end(), cacheKey.begin(), ::towlower);

    auto it = m_clusterSizeCache.find(cacheKey);
    if (it != m_clusterSizeCache.end()) return it->second;

    DWORD sectorsPerCluster = 0, bytesPerSector = 0;
    DWORD freeClusters = 0, totalClusters = 0;
    DWORD clusterSize = 0;
    if (GetDiskFreeSpaceW(root.c_str(), &sectorsPerCluster, &bytesPerSector,
                          &freeClusters, &totalClusters)) {
        clusterSize = sectorsPerCluster * bytesPerSector;
    }
    m_clusterSizeCache[cacheKey] = clusterSize;
    return clusterSize;
}

} // namespace dirsize
