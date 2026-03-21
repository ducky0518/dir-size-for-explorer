#include "resource.h"
#include "settings_dialog.h"
#include "dirsize/ipc.h"

#include <Windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <CommCtrl.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

namespace {

constexpr UINT WM_TRAYICON = WM_USER + 1;

HINSTANCE g_hInstance = nullptr;
HWND g_hWnd = nullptr;
HWND g_hSettingsDlg = nullptr;
NOTIFYICONDATAW g_nid = {};

void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON));
    wcscpy_s(g_nid.szTip, L"DirSize for Explorer");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowTrayMenu(HWND hWnd) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(hMenu, MF_STRING, IDM_SCAN_NOW, L"Scan Now");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);

    // Required for the menu to dismiss when clicking elsewhere
    SetForegroundWindow(hWnd);

    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    PostMessage(hWnd, WM_NULL, 0, 0); // See KB135788

    DestroyMenu(hMenu);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            ShowTrayMenu(hWnd);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_SETTINGS:
            if (!g_hSettingsDlg || !IsWindow(g_hSettingsDlg)) {
                g_hSettingsDlg = dirsize::ShowSettingsDialog(g_hInstance, hWnd);
                if (g_hSettingsDlg) ShowWindow(g_hSettingsDlg, SW_SHOW);
            } else {
                SetForegroundWindow(g_hSettingsDlg);
            }
            return 0;

        case IDM_SCAN_NOW: {
            dirsize::IpcStatus status;
            dirsize::SendCommand(dirsize::IpcCommand::ReloadConfig, status);
            return 0;
        }

        case IDM_ABOUT:
            MessageBoxW(hWnd,
                L"DirSize for Explorer v1.0\n\n"
                L"Adds a Total Size column to Windows Explorer\n"
                L"for viewing directory sizes.",
                L"About DirSize", MB_OK | MB_ICONINFORMATION);
            return 0;

        case IDM_EXIT:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                    LPWSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    g_hInstance = hInstance;

    // Initialize common controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_TAB_CLASSES | ICC_UPDOWN_CLASS;
    InitCommonControlsEx(&icex);

    // Initialize COM (needed for SHBrowseForFolder)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Create a hidden window for message handling
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DirSizeTrayClass";
    RegisterClassExW(&wc);

    g_hWnd = CreateWindowExW(0, L"DirSizeTrayClass", L"DirSize Tray",
                             0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        CoUninitialize();
        return 1;
    }

    AddTrayIcon(g_hWnd);

    // Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        // Forward dialog messages to the settings dialog if it's open
        if (g_hSettingsDlg && IsWindow(g_hSettingsDlg) &&
            IsDialogMessageW(g_hSettingsDlg, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
