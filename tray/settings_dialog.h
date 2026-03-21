#pragma once

#include <Windows.h>

namespace dirsize {

// Show the settings dialog (modeless). Returns the dialog HWND.
HWND ShowSettingsDialog(HINSTANCE hInstance, HWND hParent);

// Dialog procedure
INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT message,
                                 WPARAM wParam, LPARAM lParam);

} // namespace dirsize
