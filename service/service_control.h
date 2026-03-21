#pragma once

#include <Windows.h>

namespace dirsize {

// Service name used with SCM (sc create, sc start, etc.)
inline constexpr wchar_t kServiceName[] = L"DirSizeSvc";
inline constexpr wchar_t kServiceDisplayName[] = L"Directory Size Scanner";
inline constexpr wchar_t kServiceDescription[] =
    L"Calculates and caches directory sizes for the Explorer Total Size column.";

// Global stop event — signaled when the service should shut down.
extern HANDLE g_stopEvent;

// Called by ServiceMain after initialization to report running status,
// and to handle control requests (stop, shutdown, interrogate).
void ReportServiceStatus(DWORD currentState, DWORD exitCode = NO_ERROR,
                         DWORD waitHint = 0);

// The SCM control handler callback.
DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD eventType,
                                LPVOID eventData, LPVOID context);

// Register and start the service control dispatcher.
// Called from main(). Blocks until the service stops.
bool RunAsService();

// The actual ServiceMain entry point registered with SCM.
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);

} // namespace dirsize
