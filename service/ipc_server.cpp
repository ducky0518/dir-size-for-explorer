#include "ipc_server.h"
#include "scanner.h"
#include "log_buffer.h"

#include <cstring>
#include <filesystem>
#include <vector>

namespace dirsize {

namespace {

// Maximum accepted request payload. Prevents a malicious client from
// making the service allocate arbitrary amounts of memory.
constexpr uint32_t kMaxRequestPayload = 64 * 1024;

// Timeout for individual pipe reads/writes so a stalled or malicious
// client can't hang the (single-threaded) listener forever.
constexpr DWORD kIoTimeoutMs = 5000;

// The pipe handle is created with FILE_FLAG_OVERLAPPED, so all IO must go
// through OVERLAPPED calls. These helpers perform cancellable, time-limited
// transfers; stopEvent aborts immediately on service shutdown.
bool PipeTransfer(HANDLE hPipe, void* buf, DWORD len, bool write,
                  HANDLE stopEvent) {
    DWORD done = 0;
    BYTE* p = static_cast<BYTE*>(buf);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    bool result = true;
    while (done < len) {
        ResetEvent(ov.hEvent);
        DWORD chunk = 0;
        BOOL ok = write
            ? WriteFile(hPipe, p + done, len - done, &chunk, &ov)
            : ReadFile(hPipe, p + done, len - done, &chunk, &ov);

        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) { result = false; break; }

            HANDLE waits[] = { stopEvent, ov.hEvent };
            DWORD wr = WaitForMultipleObjects(
                stopEvent ? 2 : 1, stopEvent ? waits : &ov.hEvent,
                FALSE, kIoTimeoutMs);
            if (wr != (stopEvent ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0)) {
                // Stop signaled, timeout, or wait failure
                CancelIoEx(hPipe, &ov);
                GetOverlappedResult(hPipe, &ov, &chunk, TRUE);
                result = false;
                break;
            }
            if (!GetOverlappedResult(hPipe, &ov, &chunk, FALSE)) {
                result = false;
                break;
            }
        }
        if (chunk == 0) { result = false; break; }
        done += chunk;
    }

    CloseHandle(ov.hEvent);
    return result;
}

bool PipeRead(HANDLE hPipe, void* buf, DWORD len, HANDLE stopEvent) {
    return PipeTransfer(hPipe, buf, len, false, stopEvent);
}

bool PipeWrite(HANDLE hPipe, const void* buf, DWORD len, HANDLE stopEvent) {
    return PipeTransfer(hPipe, const_cast<void*>(buf), len, true, stopEvent);
}

// Bounded replacement for FlushFileBuffers before DisconnectNamedPipe.
// FlushFileBuffers blocks with NO timeout until the client consumes every
// buffered byte — a client that reads the header but never drains the
// payload could wedge the single-threaded listener forever. Instead, wait
// (time-limited, via the same overlapped helper) for the client to close
// its end: our clients close right after reading the full response, which
// completes the pending read with ERROR_BROKEN_PIPE almost immediately.
// A stalled client is simply abandoned after the IO timeout.
void WaitForClientToFinish(HANDLE hPipe, HANDLE stopEvent) {
    BYTE unused;
    PipeRead(hPipe, &unused, 1, stopEvent);
}

} // namespace

IpcServer::IpcServer(std::shared_ptr<Database> db)
    : m_db(std::move(db))
{
}

IpcServer::~IpcServer() {
    Stop();
}

void IpcServer::Start() {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_running.store(true);
    m_listenerThread = std::thread(&IpcServer::ListenerThread, this);
}

void IpcServer::Stop() {
    m_running.store(false);
    if (m_stopEvent) {
        SetEvent(m_stopEvent);
    }
    if (m_listenerThread.joinable()) {
        m_listenerThread.join();
    }
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

void IpcServer::ListenerThread() {
    // The engine runs in the user's own session; the pipe name is
    // session-scoped and the default security descriptor (creator/owner +
    // SYSTEM) is exactly what we want — only this user's processes
    // (Explorer shell extension, settings dialog) can connect. This
    // replaces the old NULL DACL, which granted everyone full control.
    std::wstring pipeName = GetPipeName();

    while (m_running.load()) {
        HANDLE hPipe = CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            64 * 1024, // Output buffer size (GetLog responses fit without
                       // the write blocking on the client's read pace)
            4096,   // Input buffer size
            0,      // Default timeout
            nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(1000); // Retry after a delay
            continue;
        }

        // Wait for a client to connect, with ability to cancel via stop event
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        BOOL connected = ConnectNamedPipe(hPipe, &overlapped);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                // Wait for either a client connection or stop signal
                HANDLE events[] = { overlapped.hEvent, m_stopEvent };
                DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);

                if (waitResult != WAIT_OBJECT_0) {
                    // Stop signal or error — cancel and clean up
                    CancelIo(hPipe);
                    CloseHandle(overlapped.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(overlapped.hEvent);
                CloseHandle(hPipe);
                continue;
            }
        }

        CloseHandle(overlapped.hEvent);

        // Handle this client (synchronously for simplicity; could use a thread pool)
        HandleClient(hPipe);
        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }
}

void IpcServer::HandleClient(HANDLE hPipe) {
    // Read request header (cancellable + time-limited; the pipe is
    // overlapped, so plain synchronous ReadFile is not valid here anyway)
    IpcRequestHeader reqHeader;
    if (!PipeRead(hPipe, &reqHeader, sizeof(reqHeader), m_stopEvent)) {
        return;
    }

    // Reject absurd payload sizes — the length field comes from an
    // untrusted client and previously drove an unbounded allocation.
    if (reqHeader.pathLengthBytes > kMaxRequestPayload) {
        return;
    }

    // Read payload if present (raw bytes — interpreted per command)
    std::vector<uint8_t> rawPayload;
    std::wstring path;
    if (reqHeader.pathLengthBytes > 0) {
        rawPayload.resize(reqHeader.pathLengthBytes);
        if (!PipeRead(hPipe, rawPayload.data(), reqHeader.pathLengthBytes,
                      m_stopEvent)) {
            return;
        }
        // For most commands, the payload is a null-terminated wchar_t path
        if (reqHeader.command != IpcCommand::GetLog) {
            auto* wchars = reinterpret_cast<const wchar_t*>(rawPayload.data());
            size_t wcharCount = reqHeader.pathLengthBytes / sizeof(wchar_t);
            if (wcharCount > 0) {
                path.assign(wchars, wcharCount - 1); // Exclude null terminator
            }
        }
    }

    // Process command
    IpcResponseHeader response;
    response.status = IpcStatus::Ok;
    response.dataLengthBytes = 0;
    bool responseSent = false;

    switch (reqHeader.command) {
    case IpcCommand::Recalculate:
        if (m_scanner && !path.empty()) {
            m_scanner->QueueRescan(path);
        } else {
            response.status = IpcStatus::Error;
        }
        break;

    case IpcCommand::GetStatus:
        if (m_scanner && m_scanner->IsScanning()) {
            response.status = IpcStatus::Busy;
        }
        break;

    case IpcCommand::ReloadConfig:
        if (m_scanner) {
            m_scanner->ReloadConfig();
        }
        // Let the host react too (e.g. restart directory watchers when
        // the watched-directory list changed).
        if (m_onReload) {
            m_onReload();
        }
        break;

    case IpcCommand::ScanNow:
        if (m_scanner) {
            m_scanner->RequestFullScan();
        }
        break;

    case IpcCommand::GetLog: {
        // Read sinceSeqNum from raw payload
        uint32_t sinceSeqNum = 0;
        if (rawPayload.size() >= sizeof(uint32_t)) {
            std::memcpy(&sinceSeqNum, rawPayload.data(), sizeof(uint32_t));
        }

        // Build status info
        ServiceStatusWire statusInfo = {};
        if (m_scanner) {
            statusInfo.isScanning = m_scanner->IsScanning() ? 1 : 0;
            statusInfo.lastScanTimestamp = m_scanner->GetLastFullScanTime();
        }
        if (m_db) {
            statusInfo.dbEntryCount = m_db->GetEntryCount();
        }
        // Get DB file size
        try {
            auto dbPath = Database::GetDefaultPath();
            statusInfo.dbSizeBytes = std::filesystem::file_size(dbPath);
        } catch (...) {
            statusInfo.dbSizeBytes = 0;
        }

        // Get current scan path (convert to UTF-8)
        std::string currentPathUtf8;
        if (m_scanner) {
            std::wstring currentPath = m_scanner->GetCurrentPath();
            if (!currentPath.empty()) {
                int needed = WideCharToMultiByte(CP_UTF8, 0, currentPath.c_str(), -1,
                                                 nullptr, 0, nullptr, nullptr);
                if (needed > 0) {
                    currentPathUtf8.resize(needed - 1);
                    WideCharToMultiByte(CP_UTF8, 0, currentPath.c_str(), -1,
                                        currentPathUtf8.data(), needed, nullptr, nullptr);
                }
            }
        }
        statusInfo.currentPathLength = static_cast<uint16_t>(currentPathUtf8.size());

        // Serialize log entries
        std::vector<uint8_t> logData;
        bool truncated = false;
        uint32_t latestSeq = GetLogBuffer().Serialize(sinceSeqNum, logData, truncated);
        if (truncated) {
            latestSeq |= 0x80000000u;
        }

        // Build full payload: latestSeqNum + ServiceStatusWire + currentPath + logData
        size_t payloadSize = sizeof(uint32_t) + sizeof(ServiceStatusWire)
                           + currentPathUtf8.size() + logData.size();
        std::vector<uint8_t> payload(payloadSize);
        size_t offset = 0;

        std::memcpy(payload.data() + offset, &latestSeq, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        std::memcpy(payload.data() + offset, &statusInfo, sizeof(ServiceStatusWire));
        offset += sizeof(ServiceStatusWire);

        if (!currentPathUtf8.empty()) {
            std::memcpy(payload.data() + offset, currentPathUtf8.data(),
                        currentPathUtf8.size());
            offset += currentPathUtf8.size();
        }

        if (!logData.empty()) {
            std::memcpy(payload.data() + offset, logData.data(), logData.size());
        }

        response.dataLengthBytes = static_cast<uint32_t>(payloadSize);
        if (PipeWrite(hPipe, &response, sizeof(response), m_stopEvent) &&
            !payload.empty()) {
            PipeWrite(hPipe, payload.data(), response.dataLengthBytes,
                      m_stopEvent);
        }
        responseSent = true;
        break;
    }

    default:
        Log(LogSeverity::Error, "Unknown IPC command: %d",
            static_cast<int>(reqHeader.command));
        response.status = IpcStatus::Error;
        break;
    }

    // Send response (unless already sent by GetLog)
    if (!responseSent) {
        PipeWrite(hPipe, &response, sizeof(response), m_stopEvent);
    }
    WaitForClientToFinish(hPipe, m_stopEvent);
}

} // namespace dirsize
