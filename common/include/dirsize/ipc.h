#pragma once

#include <cstdint>
#include <string>

namespace dirsize {

// Named pipe path for communication with the service
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\DirSizeSvc";

// IPC command codes
enum class IpcCommand : uint32_t {
    Recalculate = 1,    // Request rescan of a specific path
    GetStatus = 2,      // Query service/scanner status
    ReloadConfig = 3,   // Tell service to re-read config from registry
};

// Status codes returned by the service
enum class IpcStatus : uint32_t {
    Ok = 0,
    Error = 1,
    Busy = 2,           // Scanner is currently running
    NotFound = 3,       // Path not in watched directories
};

// Wire format for requests (followed by path data if applicable)
#pragma pack(push, 1)
struct IpcRequestHeader {
    IpcCommand command;
    uint32_t pathLengthBytes;   // Length of path in bytes (including null terminator), 0 if no path
};

struct IpcResponseHeader {
    IpcStatus status;
    uint32_t dataLengthBytes;   // Length of additional data in bytes, 0 if none
};
#pragma pack(pop)

// Client-side helper: send a command to the service and receive a response.
// Returns true on success. timeoutMs applies to the pipe connection.
bool SendCommand(IpcCommand command, const std::wstring& path,
                 IpcStatus& outStatus, uint32_t timeoutMs = 5000);

// Convenience overload for commands without a path argument.
bool SendCommand(IpcCommand command, IpcStatus& outStatus, uint32_t timeoutMs = 5000);

} // namespace dirsize
