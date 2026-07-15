#include "dir_watcher.h"
#include "scanner.h"
#include "log_buffer.h"
#include "dirsize/config.h"
#include "dirsize/path_utils.h"

#include <set>
#include <vector>

namespace {

// Strip the "\\?\" / "\\?\UNC\" prefix GetFinalPathNameByHandle returns,
// yielding a normal display path suitable for the registry.
std::wstring StripNtPrefix(const std::wstring& p) {
    if (p.size() > 8 && p.compare(0, 8, L"\\\\?\\UNC\\") == 0) {
        return L"\\\\" + p.substr(8);
    }
    if (p.size() > 4 && p.compare(0, 4, L"\\\\?\\") == 0) {
        return p.substr(4);
    }
    return p;
}

} // namespace

namespace dirsize {

DirectoryWatcher::DirectoryWatcher(Scanner& scanner)
    : m_scanner(scanner)
{
}

DirectoryWatcher::~DirectoryWatcher() {
    Stop();
}

std::wstring DirectoryWatcher::Root() const {
    std::lock_guard lock(m_rootMutex);
    return m_root;
}

bool DirectoryWatcher::Start(const std::wstring& rootPath, HANDLE stopEvent) {
    m_root = CanonicalizePath(rootPath);
    if (m_root.empty()) return false;
    m_stopEvent = stopEvent;

    m_dirHandle = CreateFileW(
        m_root.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (m_dirHandle == INVALID_HANDLE_VALUE) {
        Log(LogSeverity::Error, L"Watcher: cannot open %s (error %lu)",
            m_root.c_str(), GetLastError());
        return false;
    }

    m_running.store(true);
    m_thread = std::thread(&DirectoryWatcher::WatchThread, this);
    Log(LogSeverity::Info, L"Watching %s for changes", m_root.c_str());
    return true;
}

void DirectoryWatcher::Stop() {
    m_running.store(false);
    // The watch thread may close/reopen the handle itself (reconnect
    // logic), so handle access from this thread is mutex-guarded.
    {
        std::lock_guard lock(m_rootMutex);
        if (m_dirHandle != INVALID_HANDLE_VALUE) {
            CancelIoEx(m_dirHandle, nullptr);
        }
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    {
        std::lock_guard lock(m_rootMutex);
        if (m_dirHandle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_dirHandle);
            m_dirHandle = INVALID_HANDLE_VALUE;
        }
    }
    // Interval scans must resume for this root once we're not watching it.
    if (m_markedRealtime) {
        m_scanner.MarkRealtimeInactive(m_root);
        m_markedRealtime = false;
    }
}

void DirectoryWatcher::WatchThread() {
    // DWORD-aligned buffer as required by ReadDirectoryChangesW
    constexpr DWORD kBufferSize = 64 * 1024;
    std::vector<DWORD> alignedBuf(kBufferSize / sizeof(DWORD));
    BYTE* buffer = reinterpret_cast<BYTE*>(alignedBuf.data());

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return;

    constexpr DWORD kNotifyFilter =
        FILE_NOTIFY_CHANGE_FILE_NAME |   // create/delete/rename files
        FILE_NOTIFY_CHANGE_DIR_NAME |    // create/delete/rename directories
        FILE_NOTIFY_CHANGE_SIZE |        // file growth/truncation
        FILE_NOTIFY_CHANGE_LAST_WRITE;   // in-place modification

    constexpr DWORD kReconnectDelayMs = 60 * 1000;

    bool readPending = false;
    bool lossLogged = false;

    // SMB change-notify subscriptions die with the session (idle
    // disconnects, NAS reboots, network drops) — and previously that
    // silently killed this thread while interval scans STAYED suspended,
    // leaving the root permanently un-tracked. Now: drop the handle and
    // the real-time suspension (interval scans resume as the backstop)
    // and retry the subscription once a minute until it comes back.
    auto connectionLost = [&](DWORD err) {
        if (readPending) {
            CancelIoEx(m_dirHandle, &ov);
            DWORD ignored = 0;
            GetOverlappedResult(m_dirHandle, &ov, &ignored, TRUE);
            readPending = false;
        }
        {
            std::lock_guard lock(m_rootMutex);
            if (m_dirHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(m_dirHandle);
                m_dirHandle = INVALID_HANDLE_VALUE;
            }
        }
        if (m_markedRealtime) {
            m_scanner.MarkRealtimeInactive(m_root);
            m_markedRealtime = false;
        }
        if (!lossLogged) {
            Log(LogSeverity::Info,
                L"Watcher: change notifications lost for %s (error %lu) — "
                L"interval scans resume; retrying every 60 s",
                m_root.c_str(), err);
            lossLogged = true;
        }
    };

    while (m_running.load()) {
        // (Re)establish the directory handle if we lost it
        if (m_dirHandle == INVALID_HANDLE_VALUE) {
            HANDLE h = CreateFileW(
                m_root.c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                if (WaitForSingleObject(m_stopEvent, kReconnectDelayMs) ==
                    WAIT_OBJECT_0) break;
                continue;
            }
            {
                std::lock_guard lock(m_rootMutex);
                m_dirHandle = h;
            }
            Log(LogSeverity::Info, L"Watcher: reconnected to %s",
                m_root.c_str());
            lossLogged = false;
            m_deliveryLogged = false;

            // Reconnection is a blind window: anything that changed while
            // the subscription was down went unseen, at unknown depths. A
            // shallow rescan can't repair that (it reuses cached child
            // totals), and once the watch re-arms, real-time coverage
            // suspends the interval backstop again — so request a deep
            // reconciliation scan that bypasses the suspension.
            m_scanner.RequestReconcileScan(m_root);
        }

        DWORD bytesReturned = 0;

        if (!readPending) {
            ResetEvent(ov.hEvent);
            BOOL ok = ReadDirectoryChangesW(
                m_dirHandle, buffer, kBufferSize,
                TRUE, // recursive
                kNotifyFilter,
                &bytesReturned, &ov, nullptr);

            if (!ok && GetLastError() != ERROR_IO_PENDING) {
                // Dead handle, or a filesystem that doesn't support
                // (recursive) change notifications. Either way: interval
                // scans resume and we retry cheaply once a minute.
                connectionLost(GetLastError());
                if (WaitForSingleObject(m_stopEvent, kReconnectDelayMs) ==
                    WAIT_OBJECT_0) break;
                continue;
            }
            readPending = true;

            // The recursive watch was accepted by the filesystem — that's
            // the moment to suspend interval scans, NOT the first actual
            // notification. Waiting for a notification meant a quiet
            // share never confirmed and kept getting full interval scans
            // (hours-long on large shares) despite a healthy watcher.
            if (!m_markedRealtime) {
                m_scanner.MarkRealtimeActive(m_root);
                m_markedRealtime = true;
            }
        }

        // Wait for data or shutdown (zero CPU while idle). The 60s timeout
        // exists only to periodically verify the watched root wasn't
        // renamed — the pending read stays outstanding across timeouts.
        HANDLE waits[] = { m_stopEvent, ov.hEvent };
        DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, 60000);
        if (waitResult == WAIT_TIMEOUT) {
            CheckRootRename();
            continue;
        }
        if (waitResult != WAIT_OBJECT_0 + 1) {
            break; // Stop requested or wait error (pending IO cancelled below)
        }
        readPending = false;
        if (!GetOverlappedResult(m_dirHandle, &ov, &bytesReturned, FALSE)) {
            DWORD err = GetLastError();
            if (!m_running.load()) break;
            if (err == ERROR_NOTIFY_ENUM_DIR) {
                // Overflow variant: too many changes for the buffer,
                // details lost at unknown depths — request a deep
                // reconciliation scan and re-arm.
                m_scanner.RequestReconcileScan(m_root);
                continue;
            }
            // Anything else is a lost subscription (SMB session drop etc.)
            connectionLost(err);
            if (WaitForSingleObject(m_stopEvent, kReconnectDelayMs) ==
                WAIT_OBJECT_0) break;
            continue;
        }

        // Confirm end-to-end delivery once per (re)connection, at Normal
        // verbosity — the per-change "Rescan queued" lines are Verbose,
        // which made a healthy watcher look silent in the log.
        if (!m_deliveryLogged) {
            Log(LogSeverity::Info,
                L"Watcher: change notifications flowing for %s",
                m_root.c_str());
            m_deliveryLogged = true;
        }

        if (bytesReturned == 0) {
            // Buffer overflow: too many changes at once, details were lost
            // at unknown depths. Interval scans stay suspended while the
            // watch is healthy, so a deep reconciliation scan is the only
            // thing that actually repairs what was missed.
            Log(LogSeverity::Verbose,
                L"Watcher: notification overflow on %s", m_root.c_str());
            m_scanner.RequestReconcileScan(m_root);
        } else {
            // Collect the set of directories whose contents changed:
            // the parent directory of each reported item. Rename pairs
            // (OLD_NAME + NEW_NAME) of directories are handled specially
            // so the cached subtree is carried over instead of rescanned.
            std::set<std::wstring> affectedDirs;

            auto fullPathOf = [this](const std::wstring& relative) {
                std::wstring p = m_root;
                if (p.back() != L'\\') p += L'\\';
                return p + relative;
            };
            auto containingDirOf = [this](const std::wstring& relative) {
                size_t sep = relative.find_last_of(L'\\');
                std::wstring dir = m_root;
                if (sep != std::wstring::npos) {
                    if (dir.back() != L'\\') dir += L'\\';
                    dir += relative.substr(0, sep);
                }
                return dir;
            };

            BYTE* ptr = buffer;
            for (;;) {
                auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr);

                std::wstring relative(
                    info->FileName,
                    info->FileNameLength / sizeof(wchar_t));

                switch (info->Action) {
                case FILE_ACTION_RENAMED_OLD_NAME:
                    // Remember until the matching NEW_NAME arrives
                    // (they may even land in separate batches).
                    m_pendingRenameOld = relative;
                    break;

                case FILE_ACTION_RENAMED_NEW_NAME:
                    if (!m_pendingRenameOld.empty()) {
                        std::wstring oldFull = fullPathOf(m_pendingRenameOld);
                        std::wstring newFull = fullPathOf(relative);
                        DWORD attrs = GetFileAttributesW(newFull.c_str());
                        if (attrs != INVALID_FILE_ATTRIBUTES &&
                            (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                            // Directory rename/move: preserve cached sizes
                            // (queues both parents' rescans itself).
                            m_scanner.HandleDirectoryRename(oldFull, newFull);
                        } else {
                            // File rename: just refresh the parent(s).
                            affectedDirs.insert(
                                containingDirOf(m_pendingRenameOld));
                            affectedDirs.insert(containingDirOf(relative));
                        }
                        m_pendingRenameOld.clear();
                    } else {
                        affectedDirs.insert(containingDirOf(relative));
                    }
                    break;

                default: // added / removed / modified
                    affectedDirs.insert(containingDirOf(relative));
                    break;
                }

                if (info->NextEntryOffset == 0) break;
                ptr += info->NextEntryOffset;
            }

            for (const auto& dir : affectedDirs) {
                m_scanner.QueueRescan(dir);
            }
        }

        // Debounce: coalesce bursts (large copies, builds) instead of
        // queueing rescans per notification batch. Costs nothing when the
        // tree is idle — the read above simply blocks until a change.
        // (Changes during this window aren't lost — the kernel buffers
        // notifications for the open handle until the next read.)
        if (WaitForSingleObject(m_stopEvent, 2000) == WAIT_OBJECT_0) break;
    }

    if (readPending) {
        CancelIoEx(m_dirHandle, &ov);
        DWORD ignored = 0;
        GetOverlappedResult(m_dirHandle, &ov, &ignored, TRUE);
    }
    CloseHandle(ov.hEvent);
}

void DirectoryWatcher::CheckRootRename() {
    if (ReadRegDword(L"TrackRenames", 1) == 0) return;

    wchar_t buf[2048];
    DWORD len = GetFinalPathNameByHandleW(m_dirHandle, buf, _countof(buf),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (len == 0 || len >= _countof(buf)) return;

    std::wstring currentPath = StripNtPrefix(std::wstring(buf, len));
    std::wstring currentCanon = CanonicalizePath(currentPath);
    if (currentCanon.empty() || currentCanon == m_root) return;

    // The watched root was renamed or moved. The handle (and therefore the
    // watch) survives, so update config + cached data and keep going under
    // the new name.
    std::wstring newRoot = m_scanner.HandleWatchedRootRename(m_root, currentPath);
    {
        std::lock_guard lock(m_rootMutex);
        m_root = newRoot;
    }
}

} // namespace dirsize
