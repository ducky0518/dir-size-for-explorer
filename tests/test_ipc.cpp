#include "dirsize/ipc.h"

#include <Windows.h>

#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

using namespace dirsize;

// Minimal pipe server that echoes back an OK response for any command.
// Used to test the client-side IPC logic without the full service.
namespace {

void RunTestServer(HANDLE readyEvent, HANDLE stopEvent) {
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    HANDLE hPipe = CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, &sa);

    if (hPipe == INVALID_HANDLE_VALUE) {
        SetEvent(readyEvent);
        return;
    }

    // Signal that the server is ready
    SetEvent(readyEvent);

    // Wait for a client to connect
    ConnectNamedPipe(hPipe, nullptr);

    // Read request header
    IpcRequestHeader reqHeader;
    DWORD bytesRead = 0;
    ReadFile(hPipe, &reqHeader, sizeof(reqHeader), &bytesRead, nullptr);

    // Skip path data if any
    if (reqHeader.pathLengthBytes > 0) {
        std::vector<uint8_t> pathBuf(reqHeader.pathLengthBytes);
        ReadFile(hPipe, pathBuf.data(), reqHeader.pathLengthBytes, &bytesRead, nullptr);
    }

    // Send OK response
    IpcResponseHeader response;
    response.status = IpcStatus::Ok;
    response.dataLengthBytes = 0;

    DWORD bytesWritten = 0;
    WriteFile(hPipe, &response, sizeof(response), &bytesWritten, nullptr);
    FlushFileBuffers(hPipe);

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

} // namespace

void TestSendRecalculateCommand() {
    std::wcout << L"TestSendRecalculateCommand... ";

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Start test server in background
    std::thread serverThread(RunTestServer, readyEvent, stopEvent);

    // Wait for server to be ready
    WaitForSingleObject(readyEvent, 5000);
    Sleep(100); // Small delay to ensure pipe is listening

    IpcStatus status;
    bool ok = SendCommand(IpcCommand::Recalculate, L"C:\\Test\\Dir", status, 5000);

    assert(ok);
    assert(status == IpcStatus::Ok);

    SetEvent(stopEvent);
    serverThread.join();
    CloseHandle(readyEvent);
    CloseHandle(stopEvent);

    std::wcout << L"PASSED" << std::endl;
}

void TestSendReloadConfigCommand() {
    std::wcout << L"TestSendReloadConfigCommand... ";

    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    std::thread serverThread(RunTestServer, readyEvent, stopEvent);
    WaitForSingleObject(readyEvent, 5000);
    Sleep(100);

    IpcStatus status;
    bool ok = SendCommand(IpcCommand::ReloadConfig, status, 5000);

    assert(ok);
    assert(status == IpcStatus::Ok);

    SetEvent(stopEvent);
    serverThread.join();
    CloseHandle(readyEvent);
    CloseHandle(stopEvent);

    std::wcout << L"PASSED" << std::endl;
}

void TestSendToNonExistentService() {
    std::wcout << L"TestSendToNonExistentService... ";

    // No server running — should fail gracefully
    IpcStatus status;
    bool ok = SendCommand(IpcCommand::GetStatus, status, 500);

    assert(!ok);

    std::wcout << L"PASSED" << std::endl;
}

void TestProtocolFormat() {
    std::wcout << L"TestProtocolFormat... ";

    // Verify struct sizes match expected wire format
    assert(sizeof(IpcRequestHeader) == 8);   // command(4) + pathLength(4)
    assert(sizeof(IpcResponseHeader) == 8);  // status(4) + dataLength(4)

    std::wcout << L"PASSED" << std::endl;
}

int main() {
    std::wcout << L"=== DirSize IPC Tests ===" << std::endl;

    TestProtocolFormat();
    TestSendToNonExistentService();
    TestSendRecalculateCommand();
    TestSendReloadConfigCommand();

    std::wcout << L"\nAll tests passed!" << std::endl;
    return 0;
}
