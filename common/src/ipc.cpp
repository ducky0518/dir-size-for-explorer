#include "dirsize/ipc.h"

#include <Windows.h>
#include <vector>

namespace dirsize {

bool SendCommand(IpcCommand command, const std::wstring& path,
                 IpcStatus& outStatus, uint32_t timeoutMs) {
    // Wait for the pipe to become available
    if (!WaitNamedPipeW(kPipeName, timeoutMs)) {
        return false;
    }

    HANDLE hPipe = CreateFileW(
        kPipeName,
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Set pipe to message mode
    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    // Build and send request
    uint32_t pathBytes = path.empty() ? 0 :
        static_cast<uint32_t>((path.size() + 1) * sizeof(wchar_t));

    IpcRequestHeader header;
    header.command = command;
    header.pathLengthBytes = pathBytes;

    DWORD written = 0;
    bool ok = WriteFile(hPipe, &header, sizeof(header), &written, nullptr) &&
              written == sizeof(header);

    if (ok && pathBytes > 0) {
        ok = WriteFile(hPipe, path.c_str(), pathBytes, &written, nullptr) &&
             written == pathBytes;
    }

    // Read response
    if (ok) {
        IpcResponseHeader response;
        DWORD bytesRead = 0;
        ok = ReadFile(hPipe, &response, sizeof(response), &bytesRead, nullptr) &&
             bytesRead == sizeof(response);
        if (ok) {
            outStatus = response.status;
            // Skip any additional data for now
            if (response.dataLengthBytes > 0) {
                std::vector<uint8_t> discard(response.dataLengthBytes);
                ReadFile(hPipe, discard.data(), response.dataLengthBytes, &bytesRead, nullptr);
            }
        }
    }

    CloseHandle(hPipe);
    return ok;
}

bool SendCommand(IpcCommand command, IpcStatus& outStatus, uint32_t timeoutMs) {
    return SendCommand(command, L"", outStatus, timeoutMs);
}

} // namespace dirsize
