#include "change_journal.h"
#include "scanner.h"

#include <set>
#include <string>
#include <vector>

namespace dirsize {

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
        0,
        nullptr);

    if (m_volumeHandle == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Query the USN journal
    USN_JOURNAL_DATA_V0 journalData = {};
    DWORD bytesReturned = 0;
    if (!DeviceIoControl(
            m_volumeHandle,
            FSCTL_QUERY_USN_JOURNAL,
            nullptr, 0,
            &journalData, sizeof(journalData),
            &bytesReturned, nullptr)) {
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

    m_running.store(true);
    m_monitorThread = std::thread(&ChangeJournalMonitor::MonitorThread, this);
    return true;
}

void ChangeJournalMonitor::Stop() {
    m_running.store(false);
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

    while (m_running.load()) {
        // Check for stop signal with a short poll interval
        if (WaitForSingleObject(m_stopEvent, 2000) == WAIT_OBJECT_0) {
            break;
        }

        READ_USN_JOURNAL_DATA_V0 readData = {};
        readData.StartUsn = m_lastUsn;
        readData.ReasonMask =
            USN_REASON_FILE_CREATE |
            USN_REASON_FILE_DELETE |
            USN_REASON_DATA_OVERWRITE |
            USN_REASON_DATA_EXTEND |
            USN_REASON_DATA_TRUNCATION |
            USN_REASON_RENAME_NEW_NAME;
        readData.ReturnOnlyOnClose = FALSE;
        readData.UsnJournalID = m_journalId;

        DWORD bytesReturned = 0;
        BOOL ok = DeviceIoControl(
            m_volumeHandle,
            FSCTL_READ_USN_JOURNAL,
            &readData, sizeof(readData),
            buffer.data(), kBufferSize,
            &bytesReturned, nullptr);

        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_JOURNAL_ENTRY_DELETED) {
                // Journal wrapped around; reset to current position
                USN_JOURNAL_DATA_V0 journalData = {};
                DeviceIoControl(m_volumeHandle, FSCTL_QUERY_USN_JOURNAL,
                                nullptr, 0, &journalData, sizeof(journalData),
                                &bytesReturned, nullptr);
                m_lastUsn = journalData.NextUsn;
            }
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
        for (DWORDLONG parentRef : affectedParents) {
            std::wstring parentPath = ResolveFileReference(parentRef);
            if (!parentPath.empty()) {
                m_scanner.QueueRescan(parentPath);
            }
        }

        // Persist the bookmark
        UsnBookmark bm;
        bm.volume = std::wstring(1, m_driveLetter) + L':';
        bm.journalId = m_journalId;
        bm.lastUsn = m_lastUsn;
        m_db->UpsertUsnBookmark(bm);
    }
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

    // Get the full path
    wchar_t pathBuf[MAX_PATH * 2];
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
