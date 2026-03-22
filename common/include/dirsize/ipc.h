#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dirsize {

// Named pipe path for communication with the service
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\DirSizeSvc";

// IPC command codes
enum class IpcCommand : uint32_t {
    Recalculate = 1,    // Request rescan of a specific path
    GetStatus = 2,      // Query service/scanner status
    ReloadConfig = 3,   // Tell service to re-read config from registry
    GetLog = 4,         // Retrieve recent log entries + service status
};

// Status codes returned by the service
enum class IpcStatus : uint32_t {
    Ok = 0,
    Error = 1,
    Busy = 2,           // Scanner is currently running
    NotFound = 3,       // Path not in watched directories
};

// Log severity levels (shared between service and tray app)
enum class LogSeverity : uint8_t {
    Error = 0,
    Info = 1,
    Verbose = 2,
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

// Wire format for a single log entry in GetLog response
struct LogEntryWire {
    int64_t timestampMs;        // Unix epoch milliseconds
    LogSeverity severity;       // Error / Info / Verbose
    uint16_t messageLength;     // Length in bytes of UTF-8 message text
    // Followed by messageLength bytes of UTF-8 text
};

// Service status summary in GetLog response
struct ServiceStatusWire {
    uint8_t isScanning;         // 1 = scanning, 0 = idle
    int64_t lastScanTimestamp;  // Unix epoch ms of last completed full scan
    uint64_t dbEntryCount;      // Number of rows in dir_sizes table
    uint64_t dbSizeBytes;       // Size of dirsize.db file on disk
    uint16_t currentPathLength; // UTF-8 bytes for current scan path (0 if idle)
    // Followed by currentPathLength bytes of UTF-8 path
};
#pragma pack(pop)

// Client-side helper: send a command to the service and receive a response.
// Returns true on success. timeoutMs applies to the pipe connection.
bool SendCommand(IpcCommand command, const std::wstring& path,
                 IpcStatus& outStatus, uint32_t timeoutMs = 5000);

// Convenience overload for commands without a path argument.
bool SendCommand(IpcCommand command, IpcStatus& outStatus, uint32_t timeoutMs = 5000);

// Client-side helper: retrieve log entries from the service.
// sinceSeqNum: sequence number of last received entry (0 for first request).
// outLatestSeqNum: updated to the latest sequence number (bit 31 set if truncated).
// outData: raw payload containing ServiceStatusWire + LogEntryWire entries.
bool SendGetLog(uint32_t sinceSeqNum, IpcStatus& outStatus,
                uint32_t& outLatestSeqNum, std::vector<uint8_t>& outData,
                uint32_t timeoutMs = 5000);

} // namespace dirsize
