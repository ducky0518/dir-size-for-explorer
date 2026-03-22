#include "log_buffer.h"

#include <Windows.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace dirsize {

// ---------------------------------------------------------------------------
// LogBuffer implementation
// ---------------------------------------------------------------------------

LogBuffer::LogBuffer(size_t capacity)
    : m_capacity(capacity)
{
    m_ring.resize(capacity);
}

uint32_t LogBuffer::Append(LogSeverity severity, const std::string& message) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    Entry& entry = m_ring[m_head];
    entry.seqNum = m_nextSeqNum;
    entry.timestampMs = ms;
    entry.severity = severity;
    // Truncate very long messages to avoid bloating the buffer
    entry.message = message.size() > 4096 ? message.substr(0, 4096) : message;

    m_head = (m_head + 1) % m_capacity;
    if (m_count < m_capacity) {
        m_count++;
    }

    return m_nextSeqNum++;
}

uint32_t LogBuffer::Serialize(uint32_t sinceSeqNum, std::vector<uint8_t>& outData,
                               bool& truncated) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    outData.clear();
    truncated = false;

    if (m_count == 0) {
        return 0;
    }

    uint32_t latestSeq = m_nextSeqNum - 1;

    // Find the oldest entry's sequence number
    size_t oldestIdx = (m_count < m_capacity) ? 0 : m_head;
    uint32_t oldestSeq = m_ring[oldestIdx].seqNum;

    // If the requested sequence number is older than our oldest entry,
    // the client's data has been evicted — signal truncation
    if (sinceSeqNum > 0 && sinceSeqNum < oldestSeq) {
        truncated = true;
    }

    // Iterate from oldest to newest and serialize entries newer than sinceSeqNum
    for (size_t i = 0; i < m_count; i++) {
        size_t idx = (oldestIdx + i) % m_capacity;
        const Entry& e = m_ring[idx];

        if (e.seqNum <= sinceSeqNum) {
            continue;
        }

        // Serialize as LogEntryWire + message bytes
        LogEntryWire wire;
        wire.timestampMs = e.timestampMs;
        wire.severity = e.severity;
        wire.messageLength = static_cast<uint16_t>(
            std::min(e.message.size(), static_cast<size_t>(UINT16_MAX)));

        size_t offset = outData.size();
        outData.resize(offset + sizeof(LogEntryWire) + wire.messageLength);
        std::memcpy(outData.data() + offset, &wire, sizeof(LogEntryWire));
        std::memcpy(outData.data() + offset + sizeof(LogEntryWire),
                     e.message.data(), wire.messageLength);
    }

    return latestSeq;
}

// ---------------------------------------------------------------------------
// Singleton and convenience functions
// ---------------------------------------------------------------------------

LogBuffer& GetLogBuffer() {
    static LogBuffer instance(500);
    return instance;
}

void Log(LogSeverity severity, const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    GetLogBuffer().Append(severity, std::string(buf));
}

void Log(LogSeverity severity, const wchar_t* fmt, ...) {
    wchar_t wbuf[4096];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(wbuf, _countof(wbuf), _TRUNCATE, fmt, args);
    va_end(args);

    // Convert wide string to UTF-8
    int needed = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, nullptr, 0, nullptr, nullptr);
    if (needed > 0) {
        std::string utf8(needed - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8.data(),
                            needed, nullptr, nullptr);
        GetLogBuffer().Append(severity, utf8);
    }
}

} // namespace dirsize
