#include "ipc_server.h"
#include "scanner.h"

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

    // Read path if present
    std::wstring path;
    if (reqHeader.pathLengthBytes > 0) {
        std::vector<wchar_t> pathBuf(reqHeader.pathLengthBytes / sizeof(wchar_t));
        if (!ReadFile(hPipe, pathBuf.data(), reqHeader.pathLengthBytes, &bytesRead, nullptr) ||
            bytesRead != reqHeader.pathLengthBytes) {
            return;
        }
        path.assign(pathBuf.data(), pathBuf.size() - 1); // Exclude null terminator
    }

    // Process command
    IpcResponseHeader response;
    response.status = IpcStatus::Ok;
    response.dataLengthBytes = 0;

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

    default:
        response.status = IpcStatus::Error;
        break;
    }

    // Send response
    DWORD bytesWritten = 0;
    WriteFile(hPipe, &response, sizeof(response), &bytesWritten, nullptr);
}

} // namespace dirsize
