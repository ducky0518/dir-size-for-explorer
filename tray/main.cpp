#include "resource.h"
#include "settings_dialog.h"
#include "dirsize/ipc.h"

#include <Windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <CommCtrl.h>
#include <exdisp.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <Shldisp.h>

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
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

// --- Scanning animation state ---
HICON    g_hIconNormal = nullptr;
HICON    g_hAnimFrames[4] = {};
int      g_animFrame = 0;
bool     g_isScanning = false;
int      g_pollFailures = 0;       // consecutive failed polls
UINT     g_wmTaskbarCreated = 0;   // registered "TaskbarCreated" message

// ---------------------------------------------------------------------------
// Detect whether the current process is running elevated (admin).
// ---------------------------------------------------------------------------
bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation,
                                  &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

// ---------------------------------------------------------------------------
// Launch a copy of ourselves through the desktop shell (Explorer), which
// always runs non-elevated.  This is the Microsoft-documented way to start
// a medium-integrity process from an elevated context.
// ---------------------------------------------------------------------------
bool LaunchNonElevated(const wchar_t* path) {
    IShellWindows* shellWindows = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellWindows, nullptr,
                                  CLSCTX_LOCAL_SERVER,
                                  IID_PPV_ARGS(&shellWindows));
    if (FAILED(hr)) return false;

    VARIANT vtLoc = {};
    vtLoc.vt = VT_I4;
    vtLoc.lVal = CSIDL_DESKTOP;
    VARIANT vtEmpty = {};
    vtEmpty.vt = VT_EMPTY;
    long hwnd = 0;
    IDispatch* disp = nullptr;
    hr = shellWindows->FindWindowSW(&vtLoc, &vtEmpty, SWC_DESKTOP,
                                     &hwnd, SWFO_NEEDDISPATCH, &disp);
    shellWindows->Release();
    if (FAILED(hr) || !disp) return false;

    IServiceProvider* sp = nullptr;
    hr = disp->QueryInterface(IID_PPV_ARGS(&sp));
    disp->Release();
    if (FAILED(hr)) return false;

    IShellBrowser* browser = nullptr;
    hr = sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
    sp->Release();
    if (FAILED(hr)) return false;

    IShellView* view = nullptr;
    hr = browser->QueryActiveShellView(&view);
    browser->Release();
    if (FAILED(hr)) return false;

    IDispatch* bgDisp = nullptr;
    hr = view->GetItemObject(SVGIO_BACKGROUND, IID_PPV_ARGS(&bgDisp));
    view->Release();
    if (FAILED(hr)) return false;

    IShellFolderViewDual* folderView = nullptr;
    hr = bgDisp->QueryInterface(IID_PPV_ARGS(&folderView));
    bgDisp->Release();
    if (FAILED(hr)) return false;

    IDispatch* appDisp = nullptr;
    hr = folderView->get_Application(&appDisp);
    folderView->Release();
    if (FAILED(hr)) return false;

    IShellDispatch2* shell = nullptr;
    hr = appDisp->QueryInterface(IID_PPV_ARGS(&shell));
    appDisp->Release();
    if (FAILED(hr)) return false;

    BSTR bstrPath = SysAllocString(path);
    VARIANT vArgs = {}, vDir = {}, vOp = {}, vShow = {};
    vArgs.vt = vDir.vt = VT_EMPTY;
    vOp.vt = VT_BSTR;
    vOp.bstrVal = SysAllocString(L"open");
    vShow.vt = VT_I4;
    vShow.lVal = SW_SHOWNORMAL;

    hr = shell->ShellExecute(bstrPath, vArgs, vDir, vOp, vShow);

    SysFreeString(bstrPath);
    SysFreeString(vOp.bstrVal);
    shell->Release();
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------
// Build four animation-frame icons with a green "scan line" that sweeps
// top-to-bottom through the icon.  Each frame places the bright bar at a
// different vertical position, giving a clear scanning effect.
// ---------------------------------------------------------------------------
void CreateAnimationFrames() {
    g_hIconNormal = (HICON)LoadImageW(
        g_hInstance, MAKEINTRESOURCEW(IDI_TRAYICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_hIconNormal) return;

    ICONINFO baseInfo;
    if (!GetIconInfo(g_hIconNormal, &baseInfo)) return;

    BITMAP bm;
    GetObject(baseInfo.hbmColor, sizeof(bm), &bm);
    int cx = bm.bmWidth;
    int cy = bm.bmHeight;

    // Extract 32-bit BGRA pixel data (bottom-up row order)
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cx;
    bmi.bmiHeader.biHeight      = cy;   // positive = bottom-up
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> basePixels(cx * cy);
    HDC hdcScreen = GetDC(nullptr);
    GetDIBits(hdcScreen, baseInfo.hbmColor, 0, cy,
              basePixels.data(), &bmi, DIB_RGB_COLORS);

    // A green scan-line sweeps top → bottom in 4 steps.
    // The bar is a few pixels thick with fading edges so it reads well
    // even on a tiny 16×16 icon.
    int barHalf = (std::max)(1, cy / 12);        // 1 px radius on 16×16

    for (int frame = 0; frame < 4; frame++) {
        auto pixels = basePixels;                // work on a copy

        // Evenly space 4 line centres across the icon height
        int yCenter = (cy * (2 * frame + 1)) / 8;

        for (int dy = -barHalf; dy <= barHalf; dy++) {
            int vy = yCenter + dy;
            if (vy < 0 || vy >= cy) continue;

            // Strength falls off from centre of the bar
            float strength = 1.0f - static_cast<float>(abs(dy)) / (barHalf + 1.0f);

            for (int x = 0; x < cx; x++) {
                int idx = (cy - 1 - vy) * cx + x;       // bottom-up flip
                uint32_t px = pixels[idx];
                uint8_t a = (px >> 24) & 0xFF;
                if (a < 32) continue;                    // skip transparent

                uint8_t b =  px        & 0xFF;
                uint8_t g = (px >>  8) & 0xFF;
                uint8_t r = (px >> 16) & 0xFF;

                // Boost green channel, slightly dim red & blue (premultiplied)
                int greenBoost = static_cast<int>(120.0f * strength);
                g = static_cast<uint8_t>(
                        (std::min)(static_cast<int>(a),
                                   static_cast<int>(g) + greenBoost));
                r = static_cast<uint8_t>(r * (1.0f - 0.25f * strength));
                b = static_cast<uint8_t>(b * (1.0f - 0.25f * strength));

                pixels[idx] = (static_cast<uint32_t>(a) << 24) |
                               (static_cast<uint32_t>(r) << 16) |
                               (static_cast<uint32_t>(g) <<  8) |
                                static_cast<uint32_t>(b);
            }
        }

        void* dibBits = nullptr;
        HBITMAP hDib = CreateDIBSection(
            hdcScreen, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (hDib && dibBits) {
            memcpy(dibBits, pixels.data(), pixels.size() * sizeof(uint32_t));
            ICONINFO ni  = {};
            ni.fIcon     = TRUE;
            ni.hbmColor  = hDib;
            ni.hbmMask   = baseInfo.hbmMask;
            g_hAnimFrames[frame] = CreateIconIndirect(&ni);
        }
        if (hDib) DeleteObject(hDib);
    }

    ReleaseDC(nullptr, hdcScreen);
    DeleteObject(baseInfo.hbmColor);
    DeleteObject(baseInfo.hbmMask);
}

void DestroyAnimationFrames() {
    for (auto& h : g_hAnimFrames) {
        if (h) { DestroyIcon(h); h = nullptr; }
    }
    if (g_hIconNormal) { DestroyIcon(g_hIconNormal); g_hIconNormal = nullptr; }
}

void UpdateTrayIcon(HICON hIcon, const wchar_t* tip = nullptr) {
    g_nid.hIcon = hIcon;
    if (tip) wcscpy_s(g_nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------------------------------------------------------------------
// Start or stop the tray icon animation based on scanning state.
// Called from both PollScanStatus() and the WM_SCAN_STATE message
// (forwarded by the Logging tab) so both paths share the same logic.
// ---------------------------------------------------------------------------
void SetScanningState(bool scanning) {
    bool wasScanning = g_isScanning;
    g_isScanning = scanning;

    if (g_isScanning && !wasScanning) {
        g_animFrame = 0;
        SetTimer(g_hWnd, IDT_SCAN_ANIM, 500, nullptr);
        UpdateTrayIcon(g_hAnimFrames[0], L"DirSize \u2014 Scanning\u2026");
    } else if (!g_isScanning && wasScanning) {
        KillTimer(g_hWnd, IDT_SCAN_ANIM);
        UpdateTrayIcon(g_hIconNormal, L"DirSize for Explorer");
    }
}

// ---------------------------------------------------------------------------
// Fallback polling via lightweight GetStatus IPC — runs every 3 s.
// When the Logging tab is open it sends WM_SCAN_STATE directly, so
// this timer mainly covers the case where the settings dialog is closed.
// ---------------------------------------------------------------------------
void PollScanStatus() {
    dirsize::IpcStatus status;
    bool ok = dirsize::SendCommand(
        dirsize::IpcCommand::GetStatus, status, 2000);

    if (ok) {
        g_pollFailures = 0;
        SetScanningState(status == dirsize::IpcStatus::Busy);
    } else {
        g_pollFailures++;
        if (g_pollFailures >= 3)
            SetScanningState(false);   // service unreachable — assume idle
    }
}

void AdvanceAnimFrame() {
    g_animFrame = (g_animFrame + 1) % 4;
    if (g_hAnimFrames[g_animFrame])
        UpdateTrayIcon(g_hAnimFrames[g_animFrame]);
}

// --- Tray icon management ---

void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = g_hIconNormal;
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

    // Sent by the Logging tab every 2 s with the authoritative scanning state.
    case WM_SCAN_STATE:
        g_pollFailures = 0;
        SetScanningState(wParam != 0);
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
            dirsize::SendCommand(dirsize::IpcCommand::ScanNow, status);
            // Poll immediately so animation starts without waiting for timer
            PollScanStatus();
            return 0;
        }

        case IDM_EXIT:
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == IDT_TRAY_TIMER)
            PollScanStatus();
        else if (wParam == IDT_SCAN_ANIM)
            AdvanceAnimFrame();
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, IDT_TRAY_TIMER);
        KillTimer(hWnd, IDT_SCAN_ANIM);
        RemoveTrayIcon();
        DestroyAnimationFrames();
        PostQuitMessage(0);
        return 0;
    }

    // Explorer broadcasts "TaskbarCreated" when it restarts — re-add our icon
    if (g_wmTaskbarCreated && message == g_wmTaskbarCreated) {
        AddTrayIcon(hWnd);
        if (g_isScanning && g_hAnimFrames[g_animFrame])
            UpdateTrayIcon(g_hAnimFrames[g_animFrame], L"DirSize \u2014 Scanning\u2026");
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

    // Initialize COM (needed for SHBrowseForFolder and shell broker)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // If running elevated (e.g. launched by the MSI installer), relaunch
    // as a normal-integrity process so the tray icon is visible to Explorer.
    if (IsProcessElevated()) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        LaunchNonElevated(exePath);
        CoUninitialize();
        return 0;
    }

    // Create a hidden window for message handling
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"DirSizeTrayClass";
    RegisterClassExW(&wc);

    // Use a regular hidden window (not HWND_MESSAGE) so we receive
    // broadcast messages like TaskbarCreated when Explorer restarts.
    g_hWnd = CreateWindowExW(0, L"DirSizeTrayClass", L"DirSize Tray",
                             0, 0, 0, 0, 0, nullptr,
                             nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        CoUninitialize();
        return 1;
    }

    // Register for Explorer restart notifications
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    CreateAnimationFrames();
    AddTrayIcon(g_hWnd);

    // Poll service scanning status every 3 seconds
    SetTimer(g_hWnd, IDT_TRAY_TIMER, 3000, nullptr);

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
