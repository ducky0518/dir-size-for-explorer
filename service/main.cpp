#include "service_control.h"

int wmain(int /*argc*/, wchar_t* /*argv*/[]) {
    // When invoked by SCM, this will block until the service stops.
    // If invoked from the command line (not by SCM), StartServiceCtrlDispatcher
    // will fail — a future enhancement could add a --console flag for debugging.
    if (!dirsize::RunAsService()) {
        return 1;
    }
    return 0;
}
