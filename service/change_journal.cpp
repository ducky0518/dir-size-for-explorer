#include "change_journal.h"
#include "scanner.h"
#include "log_buffer.h"

#include <set>
#include <string>
#include <vector>

namespace dirsize {

namespace {

// Synchronous DeviceIoControl wrapper for a handle opened with
// FILE_FLAG_OVERLAPPED (a null OVERLAPPED is not allowed on such handles).
bool SyncIoctl(HANDLE hDevice, DWORD code,
               void* inBuf, DWORD inLen,
               void* outBuf, DWORD outLen,
               DWORD* bytesReturned) {
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    bool ok = DeviceIoControl(hDevice, code, inBuf, inLen,
                              outBuf, outLen, bytesReturned, &ov) != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(hDevice, &ov, bytesReturned, TRUE) != FALSE;
    }
    CloseHandle(ov.hEvent);
    return ok;
}

} // namespace

ChangeJournalMonitor::ChangeJournalMonitor(std::shared_ptr<Database> db, Scanner& scanner)
    : m_db(std::move(db))
    , m_scanner(scanner)
{
}

ChangeJournalMonitor::~ChangeJournalMonitor() {
    Stop();
}

bool ChangeJournalMonitor::Start(wchar_t driveLetter, HANDLE stopEvent) {
    m_driveLetter = driveLetter;
    m_stopEvent = stopEvent;

    // Open the volume
    wchar_t volumePath[] = L"\\\\.\\X:";
    volumePath[4] = driveLetter;

    m_volumeHandle = CreateFileW(
        volumePath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, // Allows a cancellable blocking journal read
        nullptr);

    if (m_volumeHandle == INVALID_HANDLE_VALUE) {
        Log(LogSeverity::Error, "Failed to open volume %c:", driveLetter);
        return false;
    }

    // Query the USN journal
    USN_JOURNAL_DATA_V0 journalData = {};
    DWORD bytesReturned = 0;
    if (!SyncIoctl(m_volumeHandle,
                   FSCTL_QUERY_USN_JOURNAL,
                   nullptr, 0,
                   &journalData, sizeof(journalData),
                   &bytesReturned)) {
        Log(LogSeverity::Error, "Failed to query change journal for %c:", driveLetter);
        CloseHandle(m_volumeHandle);
        m_volumeHandle = INVALID_HANDLE_VALUE;
        return false;
    }

    m_journalId = journalData.UsnJournalID;

    // Try to restore the last-read USN from the database
    std::wstring volumeKey(1, driveLetter);
    volumeKey += L':';
    auto bookmark = m_db->GetUsnBookmark(volumeKey);
    if (bookmark && bookmark->journalId == m_journalId) {
        m_lastUsn = static_cast<USN>(bookmark->lastUsn);
    } else {
        // Start from the current position (don't replay history)
        m_lastUsn = journalData.NextUsn;
    }

    Log(LogSeverity::Info, "Change journal monitor started for %c:", driveLetter);
    m_running.store(true);
    m_monitorThread = std::thread(&ChangeJournalMonitor::MonitorThread, this);
    return true;
}

void ChangeJournalMonitor::Stop() {
    m_running.store(false);
    // Wake the monitor thread if it's blocked in a journal read
    // (covers direct Stop()/destructor calls where the service stop
    // event was never signaled).
    if (m_volumeHandle != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_volumeHandle, nullptr);
    }
    if (m_monitorThread.joinable()) {
        m_monitorThread.join();
    }
    if (m_volumeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_volumeHandle);
        m_volumeHandle = INVALID_HANDLE_VALUE;
    }
}

void ChangeJournalMonitor::MonitorThread() {
    // Buffer for USN records
    constexpr DWORD kBufferSize = 64 * 1024;
    std::vector<BYTE> buffer(kBufferSize);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return;

    while (m_running.load()) {
        READ_USN_JOURNAL_DATA_V0 readData = {};
        readData.StartUsn = m_lastUsn;
        readData.ReasonMask =
            USN_REASON_FILE_CREATE |
            USN_REASON_FILE_DELETE |
            USN_REASON_DATA_OVERWRITE |
            USN_REASON_DATA_EXTEND |
            USN_REASON_DATA_TRUNCATION |
            USN_REASON_RENAME_OLD_NAME |   // Source folder of a move shrinks too
            USN_REASON_RENAME_NEW_NAME;
        readData.ReturnOnlyOnClose = FALSE;
        // Block inside the kernel until at least one record is available.
        // This removes the old 2-second polling loop: the thread sleeps
        // (zero CPU) until the filesystem actually changes.
        readData.BytesToWaitFor = 1;
        readData.Timeout = 0; // Infinite
        readData.UsnJournalID = m_journalId;

        ResetEvent(ov.hEvent);
        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            m_volumeHandle,
            FSCTL_READ_USN_JOURNAL,
            &readData, sizeof(readData),
            buffer.data(), kBufferSize,
            &bytesReturned, &ov);

        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            // Wait for data or shutdown
            HANDLE waits[] = { m_stopEvent, ov.hEvent };
            DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (waitResult != WAIT_OBJECT_0 + 1) {
                // Stop requested (or wait error) — cancel and exit
                CancelIoEx(m_volumeHandle, &ov);
                GetOverlappedResult(m_volumeHandle, &ov, &bytesReturned, TRUE);
                break;
            }
            ok = GetOverlappedResult(m_volumeHandle, &ov, &bytesReturned, FALSE);
        }

        if (!ok) {
            if (!m_running.load()) break;
            DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_ENTRY_DELETED) {
                // Journal wrapped around; reset to current position
                Log(LogSeverity::Error, "USN journal wrapped for %c: — resetting",
                    m_driveLetter);
                USN_JOURNAL_DATA_V0 journalData = {};
                if (SyncIoctl(m_volumeHandle, FSCTL_QUERY_USN_JOURNAL,
                              nullptr, 0, &journalData, sizeof(journalData),
                              &bytesReturned)) {
                    m_lastUsn = journalData.NextUsn;
                }
                continue;
            }
            // Unexpected error — back off briefly so a persistent failure
            // doesn't turn into a busy loop, but stay responsive to stop.
            if (WaitForSingleObject(m_stopEvent, 5000) == WAIT_OBJECT_0) break;
            continue;
        }

        if (bytesReturned <= sizeof(USN)) {
            // Only the next USN was returned; no new records
            USN nextUsn = *reinterpret_cast<USN*>(buffer.data());
            m_lastUsn = nextUsn;
            continue;
        }

        // Collect unique parent directory references
        std::set<DWORDLONG> affectedParents;

        // First 8 bytes are the next USN
        USN nextUsn = *reinterpret_cast<USN*>(buffer.data());
        BYTE* recordPtr = buffer.data() + sizeof(USN);
        BYTE* endPtr = buffer.data() + bytesReturned;

        while (recordPtr < endPtr) {
            auto* record = reinterpret_cast<USN_RECORD_V2*>(recordPtr);
            if (record->RecordLength == 0) break;

            affectedParents.insert(record->ParentFileReferenceNumber);

            recordPtr += record->RecordLength;
        }

        m_lastUsn = nextUsn;

        // Resolve parent references to paths and queue rescans
        int queued = 0;
        for (DWORDLONG parentRef : affectedParents) {
            std::wstring parentPath = ResolveFileReference(parentRef);
            if (!parentPath.empty()) {
                if (m_scanner.QueueRescan(parentPath))
                    queued++;
            }
        }
        if (!affectedParents.empty()) {
            Log(LogSeverity::Verbose, "USN: %d of %d changes queued on %c:",
                queued, static_cast<int>(affectedParents.size()), m_driveLetter);
        }

        // Persist the bookmark
        UsnBookmark bm;
        bm.volume = std::wstring(1, m_driveLetter) + L':';
        bm.journalId = m_journalId;
        bm.lastUsn = m_lastUsn;
        m_db->UpsertUsnBookmark(bm);

        // Debounce: during sustained write activity (large copies, builds)
        // coalesce further changes for a couple of seconds instead of
        // waking and queueing rescans per journal batch. Costs nothing
        // when the volume is idle (the read above blocks indefinitely).
        if (WaitForSingleObject(m_stopEvent, 2000) == WAIT_OBJECT_0) break;
    }

    CloseHandle(ov.hEvent);
}

std::wstring ChangeJournalMonitor::ResolveFileReference(DWORDLONG fileRefNumber) {
    // Open the file by its reference number
    FILE_ID_DESCRIPTOR fileId = {};
    fileId.dwSize = sizeof(fileId);
    fileId.Type = FileIdType;
    fileId.FileId.QuadPart = static_cast<LONGLONG>(fileRefNumber);

    HANDLE hFile = OpenFileById(
        m_volumeHandle,
        &fileId,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        FILE_FLAG_BACKUP_SEMANTICS);

    if (hFile == INVALID_HANDLE_VALUE) {
        return {};
    }

    // Get the full path (buffer large enough for long paths)
    wchar_t pathBuf[4096];
    DWORD len = GetFinalPathNameByHandleW(hFile, pathBuf, _countof(pathBuf),
                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(hFile);

    if (len == 0 || len >= _countof(pathBuf)) {
        return {};
    }

    std::wstring path(pathBuf, len);

    // GetFinalPathNameByHandle returns "\\?\C:\..." — strip the prefix
    if (path.size() > 4 && path.substr(0, 4) == L"\\\\?\\") {
        path = path.substr(4);
    }

    return path;
}

} // namespace dirsize
