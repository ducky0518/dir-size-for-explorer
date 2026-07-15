#pragma once

#include <Windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace dirsize {

class Scanner;

// Watches one directory tree for changes using ReadDirectoryChangesW and
// queues shallow rescans of the affected parent directories.
//
// This replaces the old NTFS USN change-journal monitor:
//  - needs no admin rights (the engine runs in the user's session), and
//  - works on any filesystem that supports change notifications over its
//    protocol — local NTFS/ReFS/FAT and SMB shares (Windows servers and
//    most NAS devices).
// Where change notify is unavailable or unreliable, the periodic full
// scan remains the backstop.
class DirectoryWatcher {
public:
    explicit DirectoryWatcher(Scanner& scanner);
    ~DirectoryWatcher();

    DirectoryWatcher(const DirectoryWatcher&) = delete;
    DirectoryWatcher& operator=(const DirectoryWatcher&) = delete;

    // Start watching rootPath (recursively). stopEvent aborts the watch.
    // Returns false if the directory can't be opened for notification.
    bool Start(const std::wstring& rootPath, HANDLE stopEvent);
    void Stop();

    // Current canonical root (thread-safe; may change if rename tracking
    // follows the watched folder to a new path).
    std::wstring Root() const;

private:
    void WatchThread();

    // Detects when the watched root itself was renamed/moved (the open
    // handle survives the rename, so its current path can be queried).
    // Gated by the TrackRenames setting.
    void CheckRootRename();

    Scanner& m_scanner;
    mutable std::mutex m_rootMutex;  // Guards m_root for cross-thread reads
    std::wstring m_root;
    std::wstring m_pendingRenameOld; // RENAMED_OLD_NAME awaiting its NEW_NAME
    bool m_markedRealtime = false;   // Told the scanner coverage is confirmed
    bool m_deliveryLogged = false;   // Logged first delivered notification
    HANDLE m_dirHandle = INVALID_HANDLE_VALUE;
    HANDLE m_stopEvent = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace dirsize
