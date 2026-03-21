#pragma once

#include <Windows.h>
#include <winioctl.h>

#include "dirsize/db.h"

#include <atomic>
#include <memory>
#include <thread>

namespace dirsize {

class Scanner;

// Monitors NTFS USN change journal for a single volume.
// Detects file creates, deletes, renames, and size changes,
// then queues affected parent directories for rescan.
class ChangeJournalMonitor {
public:
    ChangeJournalMonitor(std::shared_ptr<Database> db, Scanner& scanner);
    ~ChangeJournalMonitor();

    ChangeJournalMonitor(const ChangeJournalMonitor&) = delete;
    ChangeJournalMonitor& operator=(const ChangeJournalMonitor&) = delete;

    // Start monitoring the given drive letter (e.g., L'C').
    // stopEvent is signaled to shut down.
    bool Start(wchar_t driveLetter, HANDLE stopEvent);
    void Stop();

private:
    void MonitorThread();

    // Resolve a file reference number to a full path.
    std::wstring ResolveFileReference(DWORDLONG fileRefNumber);

    std::shared_ptr<Database> m_db;
    Scanner& m_scanner;

    wchar_t m_driveLetter = L'\0';
    HANDLE m_volumeHandle = INVALID_HANDLE_VALUE;
    HANDLE m_stopEvent = nullptr;

    USN m_lastUsn = 0;
    DWORDLONG m_journalId = 0;

    std::thread m_monitorThread;
    std::atomic<bool> m_running{false};
};

} // namespace dirsize
