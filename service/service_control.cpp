#include "service_control.h"
#include "scanner.h"
#include "ipc_server.h"
#include "change_journal.h"
#include "dirsize/config.h"
#include "dirsize/db.h"

#include <memory>
#include <set>

namespace dirsize {

HANDLE g_stopEvent = nullptr;

static SERVICE_STATUS g_serviceStatus = {};
static SERVICE_STATUS_HANDLE g_statusHandle = nullptr;

void ReportServiceStatus(DWORD currentState, DWORD exitCode, DWORD waitHint) {
    static DWORD checkPoint = 1;

    g_serviceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_serviceStatus.dwCurrentState = currentState;
    g_serviceStatus.dwWin32ExitCode = exitCode;
    g_serviceStatus.dwWaitHint = waitHint;

    if (currentState == SERVICE_START_PENDING) {
        g_serviceStatus.dwControlsAccepted = 0;
    } else {
        g_serviceStatus.dwControlsAccepted =
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    }

    if (currentState == SERVICE_RUNNING || currentState == SERVICE_STOPPED) {
        g_serviceStatus.dwCheckPoint = 0;
    } else {
        g_serviceStatus.dwCheckPoint = checkPoint++;
    }

    SetServiceStatus(g_statusHandle, &g_serviceStatus);
}

DWORD WINAPI ServiceCtrlHandler(DWORD control, DWORD /*eventType*/,
                                LPVOID /*eventData*/, LPVOID /*context*/) {
    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        ReportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
        SetEvent(g_stopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;

    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    g_statusHandle = RegisterServiceCtrlHandlerExW(
        kServiceName, ServiceCtrlHandler, nullptr);
    if (!g_statusHandle) return;

    ReportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    // Create the global stop event
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        ReportServiceStatus(SERVICE_STOPPED, GetLastError());
        return;
    }

    // Open the database in read-write mode
    auto db = std::make_shared<Database>();
    if (!db->Open(Database::GetDefaultPath(), false)) {
        ReportServiceStatus(SERVICE_STOPPED, ERROR_DATABASE_FAILURE);
        CloseHandle(g_stopEvent);
        return;
    }

    // Load configuration
    Config config = LoadConfig();

    // Start the scanner
    Scanner scanner(db, config);
    scanner.Start(g_stopEvent);

    // Start the IPC pipe server (needs scanner reference for recalculate/reload)
    IpcServer ipcServer(db);
    ipcServer.SetScanner(&scanner);
    ipcServer.Start();

    // Start change journal monitors (one per watched volume)
    std::vector<std::unique_ptr<ChangeJournalMonitor>> journalMonitors;
    if (config.useChangeJournal) {
        // Extract unique volume letters from watched directories
        std::set<wchar_t> volumes;
        for (const auto& dir : config.watchedDirs) {
            if (dir.size() >= 2 && dir[1] == L':') {
                volumes.insert(towupper(dir[0]));
            }
        }
        for (wchar_t vol : volumes) {
            auto monitor = std::make_unique<ChangeJournalMonitor>(db, scanner);
            if (monitor->Start(vol, g_stopEvent)) {
                journalMonitors.push_back(std::move(monitor));
            }
        }
    }

    // Service is now running
    ReportServiceStatus(SERVICE_RUNNING);

    // Wait for stop signal
    WaitForSingleObject(g_stopEvent, INFINITE);

    // Shutdown
    for (auto& monitor : journalMonitors) {
        monitor->Stop();
    }
    scanner.Stop();
    ipcServer.Stop();
    db->Close();

    CloseHandle(g_stopEvent);
    g_stopEvent = nullptr;

    ReportServiceStatus(SERVICE_STOPPED);
}

bool RunAsService() {
    SERVICE_TABLE_ENTRYW dispatchTable[] = {
        { const_cast<LPWSTR>(kServiceName), ServiceMain },
        { nullptr, nullptr }
    };
    return StartServiceCtrlDispatcherW(dispatchTable) != FALSE;
}

} // namespace dirsize
