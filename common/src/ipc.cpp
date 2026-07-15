#include "dirsize/ipc.h"

#include <Windows.h>
#include <cstring>
#include <vector>

namespace dirsize {

namespace {

// Largest response payload a client will accept. The length field comes
// from whatever process owns the pipe name — trusting it verbatim allowed
// a spoofed server to demand a multi-gigabyte allocation. Real GetLog
// payloads (status + bounded log ring) are well under a megabyte.
constexpr uint32_t kMaxResponsePayload = 16 * 1024 * 1024;

// Time-limited transfer on an OVERLAPPED pipe handle. The old client used
// plain synchronous ReadFile/WriteFile, which block forever if the server
// stalls mid-request — and some callers (context menu, settings dialog)
// run on UI threads, including inside Explorer.
bool PipeXfer(HANDLE hPipe, void* buf, DWORD len, bool write, DWORD timeoutMs) {
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

            if (WaitForSingleObject(ov.hEvent, timeoutMs) != WAIT_OBJECT_0) {
                // Timeout — abandon the request rather than hang the caller
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

bool PipeRead(HANDLE hPipe, void* buf, DWORD len, DWORD timeoutMs) {
    return PipeXfer(hPipe, buf, len, false, timeoutMs);
}

bool PipeWrite(HANDLE hPipe, const void* buf, DWORD len, DWORD timeoutMs) {
    return PipeXfer(hPipe, const_cast<void*>(buf), len, true, timeoutMs);
}

// Read and throw away len bytes without allocating them.
bool PipeDiscard(HANDLE hPipe, uint32_t len, DWORD timeoutMs) {
    uint8_t scratch[4096];
    while (len > 0) {
        DWORD chunk = (len < sizeof(scratch))
            ? static_cast<DWORD>(len) : static_cast<DWORD>(sizeof(scratch));
        if (!PipeRead(hPipe, scratch, chunk, timeoutMs)) return false;
        len -= chunk;
    }
    return true;
}

HANDLE OpenEnginePipe(uint32_t timeoutMs) {
    std::wstring pipeName = GetPipeName();

    // Wait for the pipe to become available
    if (!WaitNamedPipeW(pipeName.c_str(), timeoutMs)) {
        return INVALID_HANDLE_VALUE;
    }

    return CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
}

} // namespace

std::wstring GetPipeName() {
    DWORD sessionId = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sessionId);
    return L"\\\\.\\pipe\\DirSize_" + std::to_wstring(sessionId);
}

bool SendCommand(IpcCommand command, const std::wstring& path,
                 IpcStatus& outStatus, uint32_t timeoutMs) {
    HANDLE hPipe = OpenEnginePipe(timeoutMs);
    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Build and send request
    uint32_t pathBytes = path.empty() ? 0 :
        static_cast<uint32_t>((path.size() + 1) * sizeof(wchar_t));

    IpcRequestHeader header;
    header.command = command;
    header.pathLengthBytes = pathBytes;

    bool ok = PipeWrite(hPipe, &header, sizeof(header), timeoutMs);

    if (ok && pathBytes > 0) {
        ok = PipeWrite(hPipe, path.c_str(), pathBytes, timeoutMs);
    }

    // Read response
    if (ok) {
        IpcResponseHeader response;
        ok = PipeRead(hPipe, &response, sizeof(response), timeoutMs);
        if (ok) {
            outStatus = response.status;
            // Drain (bounded) any additional data this caller ignores
            if (response.dataLengthBytes > 0) {
                if (response.dataLengthBytes > kMaxResponsePayload) {
                    ok = false;
                } else {
                    PipeDiscard(hPipe, response.dataLengthBytes, timeoutMs);
                }
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

    HANDLE hPipe = OpenEnginePipe(timeoutMs);
    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Send request: header + sinceSeqNum as raw bytes
    IpcRequestHeader header;
    header.command = IpcCommand::GetLog;
    header.pathLengthBytes = sizeof(uint32_t);

    bool ok = PipeWrite(hPipe, &header, sizeof(header), timeoutMs);
    if (ok) {
        ok = PipeWrite(hPipe, &sinceSeqNum, sizeof(uint32_t), timeoutMs);
    }

    // Read response header + payload
    if (ok) {
        IpcResponseHeader response;
        ok = PipeRead(hPipe, &response, sizeof(response), timeoutMs);
        if (ok) {
            outStatus = response.status;
            if (response.dataLengthBytes > kMaxResponsePayload) {
                // Refuse absurd sizes instead of allocating them
                ok = false;
            } else if (response.dataLengthBytes > 0) {
                std::vector<uint8_t> payload(response.dataLengthBytes);
                ok = PipeRead(hPipe, payload.data(), response.dataLengthBytes,
                              timeoutMs);
                if (ok && payload.size() >= sizeof(uint32_t)) {
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
