#pragma once

#include <Windows.h>
#include "dirsize/config.h"

namespace dirsize {

// Controls CPU and IO priority for scanner threads to minimize impact
// on normal system usage.
class IOThrottle {
public:
    explicit IOThrottle(IOPriorityLevel level = IOPriorityLevel::Low);

    // Apply priority settings to the current thread.
    // Call this at the start of each worker thread.
    void Apply();

    // Called after scanning each directory. Inserts an artificial delay
    // based on priority level to yield CPU/IO bandwidth.
    void Checkpoint();

    void SetLevel(IOPriorityLevel level);

private:
    IOPriorityLevel m_level;
    DWORD m_dirCount = 0;
};

} // namespace dirsize
