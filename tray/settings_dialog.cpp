#include "settings_dialog.h"
#include "resource.h"
#include "dirsize/config.h"
#include "dirsize/ipc.h"

#include <CommCtrl.h>
#include <ShlObj.h>

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

void ShowTabControls(HWND hDlg, int tabIndex) {
    // Show/hide controls based on active tab
    for (int id : kScannerControls) {
        ShowWindow(GetDlgItem(hDlg, id), tabIndex == 0 ? SW_SHOW : SW_HIDE);
    }
    // Also show/hide the static labels (they have id -1, so we use
    // enumeration or just manage by tab index)
    for (int id : kDisplayControls) {
        ShowWindow(GetDlgItem(hDlg, id), tabIndex == 1 ? SW_SHOW : SW_HIDE);
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

        LoadSettingsToDialog(hDlg);
        ShowTabControls(hDlg, 0);
        return TRUE;
    }

    case WM_NOTIFY: {
        NMHDR* pnmh = reinterpret_cast<NMHDR*>(lParam);
        if (pnmh && pnmh->idFrom == IDC_TAB_CONTROL && pnmh->code == TCN_SELCHANGE) {
            int tabIndex = TabCtrl_GetCurSel(GetDlgItem(hDlg, IDC_TAB_CONTROL));
            ShowTabControls(hDlg, tabIndex);
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_OK:
            if (SaveSettingsFromDialog(hDlg)) {
                DestroyWindow(hDlg);
            }
            return TRUE;

        case IDC_BTN_CANCEL:
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
        }
        break;

    case WM_CLOSE:
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
