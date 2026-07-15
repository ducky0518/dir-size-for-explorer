#include "settings_dialog.h"
#include "resource.h"
#include "scanner.h" // In-process engine (real-time coverage display)
#include "dirsize/config.h"
#include "dirsize/ipc.h"
#include "dirsize/path_utils.h"

#include <CommCtrl.h>
#include <ShlObj.h>
#include <shellapi.h>

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
    IDC_LBL_DIR_INTERVAL, IDC_DIR_INTERVAL_EDIT, IDC_BTN_SET_INTERVAL,
    IDC_LBL_EXCLUDED_DIRS, IDC_EXCLUDED_LIST,
    IDC_BTN_ADD_EXCL, IDC_BTN_REMOVE_EXCL,
    IDC_CHK_CHANGE_JOURNAL, IDC_CHK_TRACK_RENAMES
};

// --- Watched-directories ListView helpers -------------------------------

void InitWatchedListView(HWND hDlg) {
    HWND hLv = GetDlgItem(hDlg, IDC_WATCHED_LIST);
    ListView_SetExtendedListViewStyle(hLv, LVS_EX_FULLROWSELECT);

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<LPWSTR>(L"Directory");
    col.cx = 225;
    ListView_InsertColumn(hLv, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"Interval");
    col.cx = 50;
    ListView_InsertColumn(hLv, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"Real-time");
    col.cx = 72;
    ListView_InsertColumn(hLv, 2, &col);
}

// The authoritative interval lives in the row's lParam (item data), NOT in
// the column text: the Interval column displays "—" while real-time
// coverage is active (interval scans suspended), but the configured value
// must survive for saving and as the fallback if the watcher stops.
uint32_t GetRowInterval(HWND hLv, int row) {
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    ListView_GetItem(hLv, &item);
    return static_cast<uint32_t>(item.lParam);
}

void SetRowInterval(HWND hLv, int row, uint32_t intervalMin) {
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    item.lParam = static_cast<LPARAM>(intervalMin);
    ListView_SetItem(hLv, &item);
}

void AddWatchedRow(HWND hLv, const std::wstring& path, uint32_t intervalMin) {
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = ListView_GetItemCount(hLv);
    item.pszText = const_cast<LPWSTR>(path.c_str());
    item.lParam = static_cast<LPARAM>(intervalMin);
    int idx = ListView_InsertItem(hLv, &item);

    wchar_t buf[16];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%u", intervalMin);
    ListView_SetItemText(hLv, idx, 1, buf);
}

// Refresh the live columns from engine state:
//   Real-time — "Active" once the watcher has processed a real change
//               notification (which suspends interval scans for the root)
//   Interval  — shows "—" while suspended so it's obvious which mechanism
//               is in charge; shows the configured minutes otherwise
void UpdateRealtimeColumn(HWND hDlg) {
    HWND hLv = GetDlgItem(hDlg, IDC_WATCHED_LIST);
    Scanner* scanner = GetEngineScanner();

    int count = ListView_GetItemCount(hLv);
    for (int i = 0; i < count; i++) {
        wchar_t pathBuf[1024] = {};
        ListView_GetItemText(hLv, i, 0, pathBuf, _countof(pathBuf));
        bool active = scanner && scanner->IsRealtimeActive(pathBuf);

        ListView_SetItemText(hLv, i, 2,
            const_cast<LPWSTR>(active ? L"Active" : L"—"));

        if (active) {
            ListView_SetItemText(hLv, i, 1, const_cast<LPWSTR>(L"—"));
        } else {
            wchar_t buf[16];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%u",
                         GetRowInterval(hLv, i));
            ListView_SetItemText(hLv, i, 1, buf);
        }
    }
}

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

// Control IDs for the manual scan tab
const int kManualControls[] = {
    IDC_MANUAL_LABEL, IDC_BTN_MANUAL_SCAN, IDC_MANUAL_STATUS,
    IDC_MANUAL_HIST_LABEL, IDC_MANUAL_HISTORY
};

// Control IDs for the about tab
const int kAboutControls[] = {
    IDC_ABOUT_TITLE, IDC_ABOUT_DESC, IDC_ABOUT_LINK
};

// Tab indices (order of TabCtrl_InsertItem calls in WM_INITDIALOG)
enum {
    kTabScanner = 0,
    kTabDisplay = 1,
    kTabManualScan = 2,
    kTabLogging = 3,
    kTabAbout = 4,
};

// Logging tab state.
// NOTE: these are file-scope statics, so they outlive the dialog. Each
// WM_INITDIALOG must re-sync them with the (fresh, empty) controls —
// otherwise a reopened dialog shows a blank log but keeps requesting
// "entries newer than the last one I saw", and history never reappears.
static uint32_t s_lastSeqNum = 0;
static LogSeverity s_verbosityFilter = LogSeverity::Info;

void ShowTabControls(HWND hDlg, int tabIndex) {
    // Show/hide a group; explicitly invalidate controls when shown so
    // their content paints immediately. Without this, controls layered
    // over the tab control could show stale/blank content until a focus
    // click or content change forced a repaint (most visible on the
    // Logging tab's large edit box).
    auto showGroup = [hDlg](const int* ids, size_t count, bool visible) {
        for (size_t i = 0; i < count; i++) {
            HWND hCtrl = GetDlgItem(hDlg, ids[i]);
            ShowWindow(hCtrl, visible ? SW_SHOW : SW_HIDE);
            if (visible) {
                InvalidateRect(hCtrl, nullptr, TRUE);
            }
        }
    };

    showGroup(kScannerControls, _countof(kScannerControls),
              tabIndex == kTabScanner);
    showGroup(kDisplayControls, _countof(kDisplayControls),
              tabIndex == kTabDisplay);
    showGroup(kManualControls, _countof(kManualControls),
              tabIndex == kTabManualScan);
    showGroup(kLoggingControls, _countof(kLoggingControls),
              tabIndex == kTabLogging);
    showGroup(kAboutControls, _countof(kAboutControls),
              tabIndex == kTabAbout);
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

    // Watched directories (ListView: path, per-dir interval, real-time)
    HWND hLv = GetDlgItem(hDlg, IDC_WATCHED_LIST);
    ListView_DeleteAllItems(hLv);
    for (const auto& dir : config.watchedDirs) {
        AddWatchedRow(hLv, dir.path, dir.scanIntervalMinutes);
    }
    UpdateRealtimeColumn(hDlg);

    // Excluded directories
    HWND hExclList = GetDlgItem(hDlg, IDC_EXCLUDED_LIST);
    for (const auto& dir : config.excludedDirs) {
        SendMessageW(hExclList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(dir.c_str()));
    }

    // Change watching + rename tracking checkboxes
    CheckDlgButton(hDlg, IDC_CHK_CHANGE_JOURNAL,
                   config.useChangeJournal ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(hDlg, IDC_CHK_TRACK_RENAMES,
                   config.trackRenames ? BST_CHECKED : BST_UNCHECKED);

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

    // Watched directories. Canonicalize before saving — this runs in the
    // user's session, so mapped drive letters (H:\...) resolve to their
    // UNC form (\\server\share\...) here. Stored UNC paths stay valid
    // even if the letter is remapped, and match the canonical keys the
    // scanner and shell extension use.
    // Watched directories from the ListView (path + per-dir interval).
    // Canonicalize before saving — this runs in the user's session, so
    // mapped drive letters resolve to their stable UNC form here.
    {
        HWND hLv = GetDlgItem(hDlg, IDC_WATCHED_LIST);
        int count = ListView_GetItemCount(hLv);
        for (int i = 0; i < count; i++) {
            wchar_t pathBuf[1024] = {};
            ListView_GetItemText(hLv, i, 0, pathBuf, _countof(pathBuf));
            if (!pathBuf[0]) continue;

            WatchedDir wd;
            std::wstring canonical = CanonicalizePath(pathBuf);
            wd.path = canonical.empty() ? pathBuf : canonical;

            // Interval comes from item data, not column text — the column
            // shows "—" while real-time coverage suspends interval scans.
            uint32_t minutes = GetRowInterval(hLv, i);
            wd.scanIntervalMinutes = (minutes >= 1 && minutes <= 1440)
                ? minutes : config.scanIntervalMinutes;
            config.watchedDirs.push_back(std::move(wd));
        }
    }

    // Excluded directories from the listbox
    {
        HWND hList = GetDlgItem(hDlg, IDC_EXCLUDED_LIST);
        int count = static_cast<int>(SendMessageW(hList, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; i++) {
            int len = static_cast<int>(SendMessageW(hList, LB_GETTEXTLEN, i, 0));
            if (len > 0) {
                std::wstring dir(len, L'\0');
                SendMessageW(hList, LB_GETTEXT, i,
                             reinterpret_cast<LPARAM>(dir.data()));
                std::wstring canonical = CanonicalizePath(dir);
                config.excludedDirs.push_back(canonical.empty() ? dir : canonical);
            }
        }
    }

    // Change watching + rename tracking
    config.useChangeJournal =
        IsDlgButtonChecked(hDlg, IDC_CHK_CHANGE_JOURNAL) == BST_CHECKED;
    config.trackRenames =
        IsDlgButtonChecked(hDlg, IDC_CHK_TRACK_RENAMES) == BST_CHECKED;

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

// Returns the selected folder path, or empty if cancelled.
std::wstring BrowseForFolder(HWND hDlg, const wchar_t* title) {
    wchar_t path[MAX_PATH] = {};

    BROWSEINFOW bi = {};
    bi.hwndOwner = hDlg;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    std::wstring result;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) {
            result = path;
        }
        CoTaskMemFree(pidl);
    }
    return result;
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

// ---------------------------------------------------------------------------
// Manual Scan tab helpers
// ---------------------------------------------------------------------------

static void InitManualHistoryView(HWND hDlg) {
    HWND hLv = GetDlgItem(hDlg, IDC_MANUAL_HISTORY);
    ListView_SetExtendedListViewStyle(hLv, LVS_EX_FULLROWSELECT);

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<LPWSTR>(L"Folder");
    col.cx = 170;
    ListView_InsertColumn(hLv, 0, &col);
    col.pszText = const_cast<LPWSTR>(L"Started");
    col.cx = 62;
    ListView_InsertColumn(hLv, 1, &col);
    col.pszText = const_cast<LPWSTR>(L"Finished");
    col.cx = 62;
    ListView_InsertColumn(hLv, 2, &col);
    col.pszText = const_cast<LPWSTR>(L"Result");
    col.cx = 95;
    ListView_InsertColumn(hLv, 3, &col);
    col.pszText = const_cast<LPWSTR>(L"Watched");
    col.cx = 60;
    ListView_InsertColumn(hLv, 4, &col);
}

static std::wstring FormatClockTime(int64_t epochMs) {
    if (epochMs <= 0) return L"—";
    time_t t = static_cast<time_t>(epochMs / 1000);
    struct tm localTm;
    localtime_s(&localTm, &t);
    wchar_t buf[16];
    wcsftime(buf, _countof(buf), L"%H:%M:%S", &localTm);
    return buf;
}

// Rebuild the history list + status line from the engine's live state.
static void RefreshManualTab(HWND hDlg) {
    Scanner* scanner = GetEngineScanner();
    HWND hLv = GetDlgItem(hDlg, IDC_MANUAL_HISTORY);
    ListView_DeleteAllItems(hLv);

    if (!scanner) {
        SetDlgItemTextW(hDlg, IDC_MANUAL_STATUS, L"Engine not running");
        return;
    }

    bool anyActive = false;
    auto history = scanner->GetManualScanHistory();
    for (const auto& rec : history) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(hLv);
        item.pszText = const_cast<LPWSTR>(rec.path.c_str());
        int idx = ListView_InsertItem(hLv, &item);

        ListView_SetItemText(hLv, idx, 1,
            const_cast<LPWSTR>(FormatClockTime(rec.startMs).c_str()));
        ListView_SetItemText(hLv, idx, 2,
            const_cast<LPWSTR>(FormatClockTime(rec.endMs).c_str()));

        std::wstring resultText;
        if (rec.endMs == 0) {
            resultText = (rec.startMs == 0) ? L"Queued" : L"Scanning…";
            anyActive = true;
        } else if (!rec.completed) {
            resultText = L"Aborted";
        } else {
            wchar_t buf[96];
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%s, %llu files",
                         FormatFileSize(rec.totalSize).c_str(), rec.fileCount);
            resultText = buf;
        }
        ListView_SetItemText(hLv, idx, 3,
            const_cast<LPWSTR>(resultText.c_str()));

        // Is this folder covered by a configured watched directory
        // ("full scanner"), or was this a standalone one-off scan?
        ListView_SetItemText(hLv, idx, 4,
            const_cast<LPWSTR>(rec.underWatched ? L"Yes" : L"No"));
    }

    SetDlgItemTextW(hDlg, IDC_MANUAL_STATUS,
        anyActive ? L"Scan in progress…"
                  : L"Idle — pick a folder to scan it now.");
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
        // Format last scan time. Include the date when it isn't today —
        // with baseline reuse the last scan can be days old, and a bare
        // "4:48 AM" would be misleading.
        wchar_t timeStr[48] = L"Never";
        if (statusInfo.lastScanTimestamp > 0) {
            time_t t = static_cast<time_t>(statusInfo.lastScanTimestamp / 1000);
            struct tm localTm;
            localtime_s(&localTm, &t);

            time_t nowT = time(nullptr);
            struct tm nowTm;
            localtime_s(&nowTm, &nowT);

            bool sameDay = localTm.tm_year == nowTm.tm_year &&
                           localTm.tm_yday == nowTm.tm_yday;
            wcsftime(timeStr, _countof(timeStr),
                     sameDay ? L"%I:%M %p" : L"%m/%d %I:%M %p", &localTm);
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

        // CRITICAL: push the tab control to the BOTTOM of the sibling
        // z-order. The tab-page controls are siblings layered over its
        // display area; if the tab sits above them, its repaint on every
        // selection change paints over their content (symptom: blank log
        // box until clicked). WS_CLIPSIBLINGS only clips siblings that are
        // ABOVE the tab, so the z-order must put the content on top.
        SetWindowPos(hTab, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        TCITEMW tie = {};
        tie.mask = TCIF_TEXT;

        tie.pszText = const_cast<LPWSTR>(L"Scanner");
        TabCtrl_InsertItem(hTab, kTabScanner, &tie);

        tie.pszText = const_cast<LPWSTR>(L"Display");
        TabCtrl_InsertItem(hTab, kTabDisplay, &tie);

        tie.pszText = const_cast<LPWSTR>(L"Manual Scan");
        TabCtrl_InsertItem(hTab, kTabManualScan, &tie);

        tie.pszText = const_cast<LPWSTR>(L"Logging");
        TabCtrl_InsertItem(hTab, kTabLogging, &tie);

        tie.pszText = const_cast<LPWSTR>(L"About");
        TabCtrl_InsertItem(hTab, kTabAbout, &tie);

        InitWatchedListView(hDlg);
        InitManualHistoryView(hDlg);
        LoadSettingsToDialog(hDlg);

        // Initialize About tab controls
        SetDlgItemTextW(hDlg, IDC_ABOUT_TITLE,
                        L"DirSize for Explorer  v" DIRSIZE_VERSION_WSTR);
        SetDlgItemTextW(hDlg, IDC_ABOUT_DESC,
            L"DirSize for Explorer brings folder sizes to Windows Explorer \x2014 "
            L"filling in the Size column for directories, just like it already does for individual files. "
            L"Sizes are kept current automatically in the background, so you always have an accurate "
            L"picture of what\x2019s taking up space on your drives.\r\n\r\n"
            L"Source code and documentation:");
        SetDlgItemTextW(hDlg, IDC_ABOUT_LINK,
            L"<a href=\"https://github.com/ducky0518/dir-size-for-explorer\">"
            L"https://github.com/ducky0518/dir-size-for-explorer</a>");

        // Initialize logging tab controls
        HWND hVerbosity = GetDlgItem(hDlg, IDC_LOG_VERBOSITY);
        SendMessageW(hVerbosity, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Errors only"));
        SendMessageW(hVerbosity, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Normal"));
        SendMessageW(hVerbosity, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Verbose"));
        // Combo indices match LogSeverity values (Error=0, Info=1,
        // Verbose=2). Select the persisted filter so the UI reflects the
        // filter actually in effect after a dialog reopen.
        SendMessageW(hVerbosity, CB_SETCURSEL,
                     static_cast<WPARAM>(s_verbosityFilter), 0);

        // The edit control starts empty in this new dialog instance, so
        // request the full history on the first refresh — this is what
        // makes historical entries appear without waiting for a new one.
        s_lastSeqNum = 0;

        // Set edit control text limit
        SendDlgItemMessageW(hDlg, IDC_LOG_EDIT, EM_SETLIMITTEXT, 256 * 1024, 0);

        ShowTabControls(hDlg, 0);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = reinterpret_cast<NMHDR*>(lParam);
        if (!pnmh) break;

        if (pnmh->idFrom == IDC_TAB_CONTROL && pnmh->code == TCN_SELCHANGE) {
            int tabIndex = TabCtrl_GetCurSel(GetDlgItem(hDlg, IDC_TAB_CONTROL));
            ShowTabControls(hDlg, tabIndex);

            if (tabIndex == kTabLogging) {
                // Start polling and do an immediate refresh
                SetTimer(hDlg, IDT_LOG_POLL, 2000, nullptr);
                RefreshLog(hDlg);
            } else {
                KillTimer(hDlg, IDT_LOG_POLL);
            }
            if (tabIndex == kTabManualScan) {
                // Poll while visible so running scans tick over to
                // completed without user interaction
                SetTimer(hDlg, IDT_MANUAL_POLL, 1000, nullptr);
                RefreshManualTab(hDlg);
            } else {
                KillTimer(hDlg, IDT_MANUAL_POLL);
            }
            if (tabIndex == kTabScanner) {
                // Returning to the Scanner tab: refresh the live
                // "Real-time" coverage column
                UpdateRealtimeColumn(hDlg);
            }

            // Synchronous repaint of the whole page after the switch —
            // with the tab control at the bottom of the z-order this
            // guarantees the newly shown controls end up painted last.
            RedrawWindow(hDlg, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN |
                         RDW_UPDATENOW);
        }

        // About tab hyperlink click
        if ((pnmh->code == NM_CLICK || pnmh->code == NM_RETURN) &&
            pnmh->idFrom == IDC_ABOUT_LINK) {
            NMLINK* pnml = reinterpret_cast<NMLINK*>(lParam);
            ShellExecuteW(hDlg, L"open", pnml->item.szUrl,
                          nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }

    case WM_TIMER:
        if (wParam == IDT_LOG_POLL) {
            RefreshLog(hDlg);
        } else if (wParam == IDT_MANUAL_POLL) {
            RefreshManualTab(hDlg);
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_OK:
            if (SaveSettingsFromDialog(hDlg)) {
                KillTimer(hDlg, IDT_LOG_POLL);
                KillTimer(hDlg, IDT_MANUAL_POLL);
                DestroyWindow(hDlg);
            }
            return TRUE;

        case IDC_BTN_CANCEL:
            KillTimer(hDlg, IDT_LOG_POLL);
            KillTimer(hDlg, IDT_MANUAL_POLL);
            DestroyWindow(hDlg);
            return TRUE;

        case IDC_BTN_APPLY:
            SaveSettingsFromDialog(hDlg);
            // Watchers restart on save; coverage re-confirms on the next
            // real change, so reflect the current state now.
            UpdateRealtimeColumn(hDlg);
            return TRUE;

        case IDC_RADIO_FMT_DEFAULT:
        case IDC_RADIO_FMT_AUTO: {
            BOOL enable = (LOWORD(wParam) == IDC_RADIO_FMT_AUTO);
            EnableWindow(GetDlgItem(hDlg, IDC_RADIO_SCALE_FOLDERS), enable);
            EnableWindow(GetDlgItem(hDlg, IDC_RADIO_SCALE_ALL), enable);
            return TRUE;
        }

        case IDC_BTN_ADD_DIR: {
            std::wstring dir = BrowseForFolder(hDlg,
                L"Select a directory to watch:");
            if (!dir.empty()) {
                // New rows start with the default interval from the field
                BOOL translated = FALSE;
                UINT defMin = GetDlgItemInt(hDlg, IDC_SCAN_INTERVAL,
                                            &translated, FALSE);
                if (!translated || defMin < 1 || defMin > 1440) defMin = 30;
                AddWatchedRow(GetDlgItem(hDlg, IDC_WATCHED_LIST), dir, defMin);
                UpdateRealtimeColumn(hDlg);
            }
            return TRUE;
        }

        case IDC_BTN_MANUAL_SCAN: {
            std::wstring dir = BrowseForFolder(hDlg,
                L"Select a folder to scan now:");
            if (!dir.empty()) {
                if (Scanner* scanner = GetEngineScanner()) {
                    scanner->RequestManualScan(dir);
                } else {
                    MessageBoxW(hDlg, L"The scan engine is not running.",
                                L"DirSize", MB_OK | MB_ICONWARNING);
                }
                RefreshManualTab(hDlg);
            }
            return TRUE;
        }

        case IDC_BTN_ADD_EXCL: {
            std::wstring dir = BrowseForFolder(hDlg,
                L"Select a directory to exclude from scanning:");
            if (!dir.empty()) {
                SendMessageW(GetDlgItem(hDlg, IDC_EXCLUDED_LIST),
                             LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(dir.c_str()));
            }
            return TRUE;
        }

        case IDC_BTN_REMOVE_DIR: {
            // Remove ALL selected rows (multi-select), highest index first
            HWND hLv = GetDlgItem(hDlg, IDC_WATCHED_LIST);
            std::vector<int> selected;
            int i = -1;
            while ((i = ListView_GetNextItem(hLv, i, LVNI_SELECTED)) != -1) {
                selected.push_back(i);
            }
            for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
                ListView_DeleteItem(hLv, *it);
            }
            return TRUE;
        }

        case IDC_BTN_REMOVE_EXCL: {
            // Remove ALL selected entries (extended-select listbox)
            HWND hList = GetDlgItem(hDlg, IDC_EXCLUDED_LIST);
            int selCount = static_cast<int>(
                SendMessageW(hList, LB_GETSELCOUNT, 0, 0));
            if (selCount > 0) {
                std::vector<int> items(selCount);
                SendMessageW(hList, LB_GETSELITEMS, selCount,
                             reinterpret_cast<LPARAM>(items.data()));
                for (auto it = items.rbegin(); it != items.rend(); ++it) {
                    SendMessageW(hList, LB_DELETESTRING, *it, 0);
                }
            }
            return TRUE;
        }

        case IDC_BTN_SET_INTERVAL: {
            // Apply the interval in the edit box to all selected rows
            BOOL translated = FALSE;
            UINT minutes = GetDlgItemInt(hDlg, IDC_DIR_INTERVAL_EDIT,
                                         &translated, FALSE);
            if (!translated || minutes < 1 || minutes > 1440) {
                MessageBoxW(hDlg, L"Enter an interval between 1 and 1440 minutes.",
                            L"DirSize", MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }
            HWND hLv = GetDlgItem(hDlg, IDC_WATCHED_LIST);
            int i = -1;
            while ((i = ListView_GetNextItem(hLv, i, LVNI_SELECTED)) != -1) {
                SetRowInterval(hLv, i, minutes);
            }
            // Re-render the Interval column (respects the "—" display
            // for rows with active real-time coverage)
            UpdateRealtimeColumn(hDlg);
            return TRUE;
        }

        case IDC_BTN_LOG_CLEAR:
            // Clear the display only. Deliberately do NOT reset
            // s_lastSeqNum — resetting it made the next 2s poll refetch
            // the entire history, "un-clearing" the log immediately.
            SetDlgItemTextW(hDlg, IDC_LOG_EDIT, L"");
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
        KillTimer(hDlg, IDT_MANUAL_POLL);
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
