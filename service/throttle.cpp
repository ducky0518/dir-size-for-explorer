#include "throttle.h"

namespace dirsize {

IOThrottle::IOThrottle(IOPriorityLevel level) : m_level(level) {}

void IOThrottle::Apply() {
    switch (m_level) {
    case IOPriorityLevel::VeryLow:
        // THREAD_MODE_BACKGROUND_BEGIN sets both CPU and IO priority to low
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        break;
    case IOPriorityLevel::Low:
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        break;
    case IOPriorityLevel::Normal:
        // No throttling
        break;
    }
    m_dirCount = 0;
}

void IOThrottle::Checkpoint() {
    m_dirCount++;

    // Yield periodically based on priority level
    switch (m_level) {
    case IOPriorityLevel::VeryLow:
        // Sleep every 10 directories to heavily throttle
        if (m_dirCount % 10 == 0) {
            Sleep(50);
        }
        break;
    case IOPriorityLevel::Low:
        // Sleep every 50 directories for light throttling
        if (m_dirCount % 50 == 0) {
            Sleep(10);
        }
        break;
    case IOPriorityLevel::Normal:
        // Just yield the timeslice occasionally
        if (m_dirCount % 100 == 0) {
            Sleep(0);
        }
        break;
    }
}

void IOThrottle::SetLevel(IOPriorityLevel level) {
    m_level = level;
}

} // namespace dirsize
