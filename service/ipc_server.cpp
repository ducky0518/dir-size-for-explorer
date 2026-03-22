#include "ipc_server.h"
#include "scanner.h"
#include "log_buffer.h"

#include <filesystem>
#include <vector>

namespace dirsize {

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
    while (m_running.load()) {
        // Create a new pipe instance with security that allows
        // authenticated users to connect.
        SECURITY_DESCRIPTOR sd;
        InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE); // Allow all access

        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;

        HANDLE hPipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096,   // Output buffer size
            4096,   // Input buffer size
            0,      // Default timeout
            &sa);

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
    // Read request header
    IpcRequestHeader reqHeader;
    DWORD bytesRead = 0;
    if (!ReadFile(hPipe, &reqHeader, sizeof(reqHeader), &bytesRead, nullptr) ||
        bytesRead != sizeof(reqHeader)) {
        return;
    }

    // Read payload if present (raw bytes — interpreted per command)
    std::vector<uint8_t> rawPayload;
    std::wstring path;
    if (reqHeader.pathLengthBytes > 0) {
        rawPayload.resize(reqHeader.pathLengthBytes);
        if (!ReadFile(hPipe, rawPayload.data(), reqHeader.pathLengthBytes, &bytesRead, nullptr) ||
            bytesRead != reqHeader.pathLengthBytes) {
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
        DWORD bytesWritten = 0;
        WriteFile(hPipe, &response, sizeof(response), &bytesWritten, nullptr);
        if (!payload.empty()) {
            WriteFile(hPipe, payload.data(), response.dataLengthBytes,
                      &bytesWritten, nullptr);
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
        DWORD bytesWritten = 0;
        WriteFile(hPipe, &response, sizeof(response), &bytesWritten, nullptr);
    }
}

} // namespace dirsize
