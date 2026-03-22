#include "settings_dialog.h"
#include "resource.h"
#include "dirsize/config.h"
#include "dirsize/ipc.h"

#include <CommCtrl.h>
#include <ShlObj.h>

#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace dirsize {

namespace {

// Control IDs for the scanner tab
const int kScannerControls[] = {
    IDC_LBL_SCAN_INTERVAL, IDC_SCAN_INTERVAL, IDC_SPIN_INTERVAL,
    IDC_LBL_IO_PRIORITY, IDC_IO_PRIORITY,
    IDC_LBL_WATCHED_DIRS, IDC_WATCHED_LIST,
    IDC_BTN_ADD_DIR, IDC_BTN_REMOVE_DIR,
    IDC_CHK_CHANGE_JOURNAL
};

// Control IDs for the display tab
const int kDisplayControls[] = {
    IDC_GROUP_METRIC, IDC_RADIO_LOGICAL, IDC_RADIO_ALLOC,
    IDC_GROUP_FORMAT, IDC_RADIO_FMT_DEFAULT, IDC_RADIO_FMT_AUTO,
    IDC_RADIO_SCALE_FOLDERS, IDC_RADIO_SCALE_ALL
};

// Control IDs for the logging tab
const int kLoggingControls[] = {
    IDC_LOG_STATUS, IDC_LOG_EDIT,
    IDC_BTN_LOG_CLEAR, IDC_BTN_LOG_COPY,
    IDC_LBL_LOG_VERBOSITY, IDC_LOG_VERBOSITY
};

// Logging tab state
static uint32_t s_lastSeqNum = 0;
static LogSeverity s_verbosityFilter = LogSeverity::Info;

void ShowTabControls(HWND hDlg, int tabIndex) {
    for (int id : kScannerControls) {
        ShowWindow(GetDlgItem(hDlg, id), tabIndex == 0 ? SW_SHOW : SW_HIDE);
    }
    for (int id : kDisplayControls) {
        ShowWindow(GetDlgItem(hDlg, id), tabIndex == 1 ? SW_SHOW : SW_HIDE);
    }
    for (int id : kLoggingControls) {
        ShowWindow(GetDlgItem(hDlg, id), tabIndex == 2 ? SW_SHOW : SW_HIDE);
    }
}

void LoadSettingsToDialog(HWND hDlg) {
    Config config = LoadConfig();

    // Scan interval
    SetDlgItemInt(hDlg, IDC_SCAN_INTERVAL, config.scanIntervalMinutes, FALSE);
    SendDlgItemMessageW(hDlg, IDC_SPIN_INTERVAL, UDM_SETRANGE32, 1, 1440);

    // IO Priority combo
    HWND hCombo = GetDlgItem(hDlg, IDC_IO_PRIORITY);
    SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Very Low"));
    SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low"));
    SendMessageW(hCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal"));
    SendMessageW(hCombo, CB_SETCURSEL, static_cast<WPARAM>(config.ioPriority), 0);

    // Watched directories
    HWND hList = GetDlgItem(hDlg, IDC_WATCHED_LIST);
    for (const auto& dir : config.watchedDirs) {
        SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(dir.c_str()));
    }

    // Change journal checkbox
    CheckDlgButton(hDlg, IDC_CHK_CHANGE_JOURNAL,
                   config.useChangeJournal ? BST_CHECKED : BST_UNCHECKED);

    // Size metric
    CheckRadioButton(hDlg, IDC_RADIO_LOGICAL, IDC_RADIO_ALLOC,
                     config.sizeMetric == SizeMetric::AllocationSize
                         ? IDC_RADIO_ALLOC : IDC_RADIO_LOGICAL);

    // Display format
    CheckRadioButton(hDlg, IDC_RADIO_FMT_DEFAULT, IDC_RADIO_FMT_AUTO,
                     config.displayFormat == DisplayFormat::AutoScale
                         ? IDC_RADIO_FMT_AUTO : IDC_RADIO_FMT_DEFAULT);

    // Auto-scale scope (folders only vs files+folders)
    CheckRadioButton(hDlg, IDC_RADIO_SCALE_FOLDERS, IDC_RADIO_SCALE_ALL,
                     config.autoScaleFoldersOnly
                         ? IDC_RADIO_SCALE_FOLDERS : IDC_RADIO_SCALE_ALL);
    // Enable sub-options only when auto-scale is selected
    BOOL enableScope = (config.displayFormat == DisplayFormat::AutoScale);
    EnableWindow(GetDlgItem(hDlg, IDC_RADIO_SCALE_FOLDERS), enableScope);
    EnableWindow(GetDlgItem(hDlg, IDC_RADIO_SCALE_ALL), enableScope);
}

bool SaveSettingsFromDialog(HWND hDlg) {
    Config config;

    // Scan interval
    BOOL translated = FALSE;
    config.scanIntervalMinutes = GetDlgItemInt(hDlg, IDC_SCAN_INTERVAL, &translated, FALSE);
    if (!translated || config.scanIntervalMinutes == 0) {
        config.scanIntervalMinutes = 30;
    }

    // IO Priority
    int sel = static_cast<int>(SendDlgItemMessageW(hDlg, IDC_IO_PRIORITY, CB_GETCURSEL, 0, 0));
    config.ioPriority = (sel >= 0 && sel <= 2)
        ? static_cast<IOPriorityLevel>(sel) : IOPriorityLevel::Low;

    // Watched directories
    HWND hList = GetDlgItem(hDlg, IDC_WATCHED_LIST);
    int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; i++) {
        int len = static_cast<int>(SendMessageW(hList, LB_GETTEXTLEN, i, 0));
        if (len > 0) {
            std::wstring dir(len, L'\0');
            SendMessageW(hList, LB_GETTEXT, i, reinterpret_cast<LPARAM>(dir.data()));
            config.watchedDirs.push_back(dir);
        }
    }

    // Change journal
    config.useChangeJournal =
        IsDlgButtonChecked(hDlg, IDC_CHK_CHANGE_JOURNAL) == BST_CHECKED;

    // Size metric
    config.sizeMetric =
        IsDlgButtonChecked(hDlg, IDC_RADIO_ALLOC) == BST_CHECKED
            ? SizeMetric::AllocationSize : SizeMetric::LogicalSize;

    // Display format
    config.displayFormat =
        IsDlgButtonChecked(hDlg, IDC_RADIO_FMT_AUTO) == BST_CHECKED
            ? DisplayFormat::AutoScale : DisplayFormat::ExplorerDefault;

    // Auto-scale scope
    config.autoScaleFoldersOnly =
        IsDlgButtonChecked(hDlg, IDC_RADIO_SCALE_ALL) != BST_CHECKED;

    if (!SaveConfig(config)) {
        MessageBoxW(hDlg, L"Failed to save settings. Make sure the application "
                    L"is running with appropriate permissions.",
                    L"DirSize Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Tell the service to reload configuration
    IpcStatus status;
    SendCommand(IpcCommand::ReloadConfig, status);

    return true;
}

void BrowseForFolder(HWND hDlg) {
    wchar_t path[MAX_PATH] = {};

    BROWSEINFOW bi = {};
    bi.hwndOwner = hDlg;
    bi.lpszTitle = L"Select a directory to watch:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) {
            HWND hList = GetDlgItem(hDlg, IDC_WATCHED_LIST);
            SendMessageW(hList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path));
        }
        CoTaskMemFree(pidl);
    }
}

// ---------------------------------------------------------------------------
// Logging tab helpers
// ---------------------------------------------------------------------------

static void AppendLogText(HWND hEdit, const wchar_t* text) {
    int len = GetWindowTextLengthW(hEdit);
    // Prevent unbounded growth — clear if over 256K chars
    if (len > 256 * 1024) {
        SetWindowTextW(hEdit, L"");
        len = 0;
    }
    SendMessageW(hEdit, EM_SETSEL, len, len);
    SendMessageW(hEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text));
}

static std::wstring Utf8ToWide(const char* utf8, int len) {
    if (!utf8 || len == 0) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, result.data(), size);
    return result;
}

static std::wstring FormatFileSize(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes >= 1024ULL * 1024 * 1024) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.1f GB",
                     bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.1f MB",
                     bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.1f KB",
                     bytes / 1024.0);
    } else {
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%llu bytes", bytes);
    }
    return buf;
}

static void RefreshLog(HWND hDlg) {
    IpcStatus status;
    uint32_t latestSeqNum = 0;
    std::vector<uint8_t> data;

    if (!SendGetLog(s_lastSeqNum, status, latestSeqNum, data, 1500)) {
        // Don't overwrite a good status with "not connected" on a transient failure
        // Only show "not connected" if we've never received data
        if (s_lastSeqNum == 0) {
            SetDlgItemTextW(hDlg, IDC_LOG_STATUS, L"Service not connected");
        }
        return;
    }

    // Check truncation bit
    bool truncated = (latestSeqNum & 0x80000000u) != 0;
    latestSeqNum &= 0x7FFFFFFFu;

    if (truncated) {
        SetDlgItemTextW(hDlg, IDC_LOG_EDIT, L"");
    }
    s_lastSeqNum = latestSeqNum;

    if (data.size() < sizeof(ServiceStatusWire)) {
        return;
    }

    // Parse ServiceStatusWire
    size_t offset = 0;
    ServiceStatusWire statusInfo;
    std::memcpy(&statusInfo, data.data() + offset, sizeof(ServiceStatusWire));
    offset += sizeof(ServiceStatusWire);

    // Read current scan path
    std::wstring currentPath;
    if (statusInfo.currentPathLength > 0 &&
        offset + statusInfo.currentPathLength <= data.size()) {
        currentPath = Utf8ToWide(
            reinterpret_cast<const char*>(data.data() + offset),
            statusInfo.currentPathLength);
        offset += statusInfo.currentPathLength;
    }

    // Build status text
    wchar_t statusText[512];
    if (statusInfo.isScanning && !currentPath.empty()) {
        _snwprintf_s(statusText, _countof(statusText), _TRUNCATE,
            L"Status: Scanning %s | DB: %llu entries (%s)",
            currentPath.c_str(), statusInfo.dbEntryCount,
            FormatFileSize(statusInfo.dbSizeBytes).c_str());
    } else {
        // Format last scan time
        wchar_t timeStr[32] = L"Never";
        if (statusInfo.lastScanTimestamp > 0) {
            time_t t = static_cast<time_t>(statusInfo.lastScanTimestamp / 1000);
            struct tm localTm;
            localtime_s(&localTm, &t);
            wcsftime(timeStr, _countof(timeStr), L"%I:%M %p", &localTm);
        }
        _snwprintf_s(statusText, _countof(statusText), _TRUNCATE,
            L"Status: Idle | DB: %llu entries (%s) | Last scan: %s",
            statusInfo.dbEntryCount,
            FormatFileSize(statusInfo.dbSizeBytes).c_str(),
            timeStr);
    }
    SetDlgItemTextW(hDlg, IDC_LOG_STATUS, statusText);

    // Notify the main tray window so the icon animation stays in sync
    HWND hOwner = GetWindow(hDlg, GW_OWNER);
    if (hOwner)
        PostMessage(hOwner, WM_SCAN_STATE, statusInfo.isScanning, 0);

    // Parse and display log entries
    HWND hEdit = GetDlgItem(hDlg, IDC_LOG_EDIT);
    std::wstring appendBuf;

    while (offset + sizeof(LogEntryWire) <= data.size()) {
        LogEntryWire wire;
        std::memcpy(&wire, data.data() + offset, sizeof(LogEntryWire));
        offset += sizeof(LogEntryWire);

        if (offset + wire.messageLength > data.size()) {
            break;
        }

        std::string msgUtf8(
            reinterpret_cast<const char*>(data.data() + offset),
            wire.messageLength);
        offset += wire.messageLength;

        // Apply verbosity filter (client-side)
        if (static_cast<uint8_t>(wire.severity) >
            static_cast<uint8_t>(s_verbosityFilter)) {
            continue;
        }

        // Format timestamp as HH:MM:SS
        time_t t = static_cast<time_t>(wire.timestampMs / 1000);
        struct tm localTm;
        localtime_s(&localTm, &t);
        wchar_t timeStr[16];
        wcsftime(timeStr, _countof(timeStr), L"%H:%M:%S", &localTm);

        // Severity label
        const wchar_t* sevLabel = L"";
        if (wire.severity == LogSeverity::Error) sevLabel = L"ERR  ";
        else if (wire.severity == LogSeverity::Verbose) sevLabel = L"DBG  ";

        std::wstring msgWide = Utf8ToWide(msgUtf8.c_str(),
                                           static_cast<int>(msgUtf8.size()));

        appendBuf += L"[";
        appendBuf += timeStr;
        appendBuf += L"] ";
        appendBuf += sevLabel;
        appendBuf += msgWide;
        appendBuf += L"\r\n";
    }

    if (!appendBuf.empty()) {
        AppendLogText(hEdit, appendBuf.c_str());
    }
}

} // namespace

INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT message,
                                 WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        // Set up tab control
        HWND hTab = GetDlgItem(hDlg, IDC_TAB_CONTROL);
        TCITEMW tie = {};
        tie.mask = TCIF_TEXT;

        tie.pszText = const_cast<LPWSTR>(L"Scanner");
        TabCtrl_InsertItem(hTab, 0, &tie);

        tie.pszText = const_cast<LPWSTR>(L"Display");
        TabCtrl_InsertItem(hTab, 1, &tie);

        tie.pszText = const_cast<LPWSTR>(L"Logging");
        TabCtrl_InsertItem(hTab, 2, &tie);

        LoadSettingsToDialog(hDlg);

        // Initialize logging tab controls
        HWND hVerbosity = GetDlgItem(hDlg, IDC_LOG_VERBOSITY);
        SendMessageW(hVerbosity, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Errors only"));
        SendMessageW(hVerbosity, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Normal"));
        SendMessageW(hVerbosity, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Verbose"));
        SendMessageW(hVerbosity, CB_SETCURSEL, 1, 0);  // Default: Normal

        // Set edit control text limit
        SendDlgItemMessageW(hDlg, IDC_LOG_EDIT, EM_SETLIMITTEXT, 256 * 1024, 0);

        ShowTabControls(hDlg, 0);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = reinterpret_cast<NMHDR*>(lParam);
        if (pnmh && pnmh->idFrom == IDC_TAB_CONTROL && pnmh->code == TCN_SELCHANGE) {
            int tabIndex = TabCtrl_GetCurSel(GetDlgItem(hDlg, IDC_TAB_CONTROL));
            ShowTabControls(hDlg, tabIndex);

            if (tabIndex == 2) {
                // Start polling and do an immediate refresh
                SetTimer(hDlg, IDT_LOG_POLL, 2000, nullptr);
                RefreshLog(hDlg);
            } else {
                KillTimer(hDlg, IDT_LOG_POLL);
            }
        }
        break;
    }

    case WM_TIMER:
        if (wParam == IDT_LOG_POLL) {
            RefreshLog(hDlg);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_OK:
            if (SaveSettingsFromDialog(hDlg)) {
                KillTimer(hDlg, IDT_LOG_POLL);
                DestroyWindow(hDlg);
            }
            return TRUE;

        case IDC_BTN_CANCEL:
            KillTimer(hDlg, IDT_LOG_POLL);
            DestroyWindow(hDlg);
            return TRUE;

        case IDC_BTN_APPLY:
            SaveSettingsFromDialog(hDlg);
            return TRUE;

        case IDC_RADIO_FMT_DEFAULT:
        case IDC_RADIO_FMT_AUTO: {
            BOOL enable = (LOWORD(wParam) == IDC_RADIO_FMT_AUTO);
            EnableWindow(GetDlgItem(hDlg, IDC_RADIO_SCALE_FOLDERS), enable);
            EnableWindow(GetDlgItem(hDlg, IDC_RADIO_SCALE_ALL), enable);
            return TRUE;
        }

        case IDC_BTN_ADD_DIR:
            BrowseForFolder(hDlg);
            return TRUE;

        case IDC_BTN_REMOVE_DIR: {
            HWND hList = GetDlgItem(hDlg, IDC_WATCHED_LIST);
            int sel = static_cast<int>(SendMessageW(hList, LB_GETCURSEL, 0, 0));
            if (sel != LB_ERR) {
                SendMessageW(hList, LB_DELETESTRING, sel, 0);
            }
            return TRUE;
        }

        case IDC_BTN_LOG_CLEAR:
            SetDlgItemTextW(hDlg, IDC_LOG_EDIT, L"");
            s_lastSeqNum = 0;
            return TRUE;

        case IDC_BTN_LOG_COPY: {
            HWND hEdit = GetDlgItem(hDlg, IDC_LOG_EDIT);
            SendMessageW(hEdit, EM_SETSEL, 0, -1);
            SendMessageW(hEdit, WM_COPY, 0, 0);
            SendMessageW(hEdit, EM_SETSEL, -1, -1);
            return TRUE;
        }

        case IDC_LOG_VERBOSITY:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = static_cast<int>(
                    SendDlgItemMessageW(hDlg, IDC_LOG_VERBOSITY,
                                        CB_GETCURSEL, 0, 0));
                s_verbosityFilter = static_cast<LogSeverity>(sel);
                // Clear and re-fetch to apply new filter
                SetDlgItemTextW(hDlg, IDC_LOG_EDIT, L"");
                s_lastSeqNum = 0;
                RefreshLog(hDlg);
            }
            return TRUE;
        }
        break;

    case WM_CLOSE:
        KillTimer(hDlg, IDT_LOG_POLL);
        DestroyWindow(hDlg);
        return TRUE;
    }

    return FALSE;
}

HWND ShowSettingsDialog(HINSTANCE hInstance, HWND hParent) {
    return CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_SETTINGS),
                              hParent, SettingsDlgProc, 0);
}

} // namespace dirsize
