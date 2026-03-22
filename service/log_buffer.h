#pragma once

#include "dirsize/ipc.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dirsize {

// Thread-safe circular buffer holding the last N log entries.
// Used by the service to record operational events; retrieved by the
// tray app via the GetLog IPC command.
class LogBuffer {
public:
    explicit LogBuffer(size_t capacity = 500);

    // Append a log entry. Returns the assigned sequence number.
    uint32_t Append(LogSeverity severity, const std::string& message);

    // Serialize all entries with seqNum > sinceSeqNum into a byte vector
    // of LogEntryWire records. Returns the latest sequence number.
    // Sets truncated=true if sinceSeqNum entries have been evicted
    // (caller should clear its display and re-render).
    uint32_t Serialize(uint32_t sinceSeqNum, std::vector<uint8_t>& outData,
                       bool& truncated) const;

private:
    struct Entry {
        uint32_t seqNum = 0;
        int64_t timestampMs = 0;
        LogSeverity severity = LogSeverity::Info;
        std::string message;    // UTF-8
    };

    mutable std::mutex m_mutex;
    std::vector<Entry> m_ring;
    size_t m_capacity;
    size_t m_head = 0;          // Next write position
    size_t m_count = 0;         // Current number of entries (<= capacity)
    uint32_t m_nextSeqNum = 1;  // Monotonically increasing
};

// Singleton accessor — the LogBuffer lives for the lifetime of the service.
LogBuffer& GetLogBuffer();

// Convenience logging functions. Format the message and append to the
// global ring buffer. The wide-char overload converts to UTF-8 internally.
void Log(LogSeverity severity, const char* fmt, ...);
void Log(LogSeverity severity, const wchar_t* fmt, ...);

} // namespace dirsize
