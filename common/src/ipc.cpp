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

bool SendGetLog(uint32_t sinceSeqNum, IpcStatus& outStatus,
                uint32_t& outLatestSeqNum, std::vector<uint8_t>& outData,
                uint32_t timeoutMs) {
    outData.clear();
    outLatestSeqNum = 0;

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

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    // Send request: header + sinceSeqNum as raw bytes
    IpcRequestHeader header;
    header.command = IpcCommand::GetLog;
    header.pathLengthBytes = sizeof(uint32_t);

    DWORD written = 0;
    bool ok = WriteFile(hPipe, &header, sizeof(header), &written, nullptr) &&
              written == sizeof(header);

    if (ok) {
        ok = WriteFile(hPipe, &sinceSeqNum, sizeof(uint32_t), &written, nullptr) &&
             written == sizeof(uint32_t);
    }

    // Read response header
    if (ok) {
        IpcResponseHeader response;
        DWORD bytesRead = 0;
        ok = ReadFile(hPipe, &response, sizeof(response), &bytesRead, nullptr) &&
             bytesRead == sizeof(response);
        if (ok) {
            outStatus = response.status;
            // Read the data payload
            if (response.dataLengthBytes > 0) {
                std::vector<uint8_t> payload(response.dataLengthBytes);
                DWORD totalRead = 0;
                while (totalRead < response.dataLengthBytes) {
                    DWORD chunkRead = 0;
                    if (!ReadFile(hPipe, payload.data() + totalRead,
                                  response.dataLengthBytes - totalRead,
                                  &chunkRead, nullptr) || chunkRead == 0) {
                        ok = false;
                        break;
                    }
                    totalRead += chunkRead;
                }
                if (ok && totalRead >= sizeof(uint32_t)) {
                    std::memcpy(&outLatestSeqNum, payload.data(), sizeof(uint32_t));
                    outData.assign(payload.begin() + sizeof(uint32_t), payload.end());
                }
            }
        }
    }

    CloseHandle(hPipe);
    return ok;
}

} // namespace dirsize
