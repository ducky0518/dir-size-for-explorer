#pragma once

#include <Windows.h>

namespace dirsize {

class Scanner;

// Implemented in main.cpp — the in-process scan engine's scanner, used by
// the settings dialog to display per-directory real-time coverage.
Scanner* GetEngineScanner();

// Show the settings dialog (modeless). Returns the dialog HWND.
HWND ShowSettingsDialog(HINSTANCE hInstance, HWND hParent);

// Dialog procedure
INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT message,
                                 WPARAM wParam, LPARAM lParam);

} // namespace dirsize
