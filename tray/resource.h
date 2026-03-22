#pragma once

// Tray icon
#define IDI_TRAYICON            101

// Settings dialog
#define IDD_SETTINGS            200
#define IDC_TAB_CONTROL         201

// Scanner tab controls
#define IDC_SCAN_INTERVAL       210
#define IDC_SPIN_INTERVAL       211
#define IDC_WATCHED_LIST        212
#define IDC_BTN_ADD_DIR         213
#define IDC_BTN_REMOVE_DIR      214
#define IDC_IO_PRIORITY         215
#define IDC_CHK_CHANGE_JOURNAL  216
#define IDC_LBL_SCAN_INTERVAL   217
#define IDC_LBL_IO_PRIORITY     218
#define IDC_LBL_WATCHED_DIRS    219

// Display tab controls — Size Metric group
#define IDC_GROUP_METRIC        222
#define IDC_RADIO_LOGICAL       223
#define IDC_RADIO_ALLOC         224

// Display tab controls — Column Formatting group
#define IDC_GROUP_FORMAT        225
#define IDC_RADIO_FMT_DEFAULT   226
#define IDC_RADIO_FMT_AUTO      227
#define IDC_RADIO_SCALE_FOLDERS 228
#define IDC_RADIO_SCALE_ALL     229

// Buttons
#define IDC_BTN_OK              230
#define IDC_BTN_CANCEL          231
#define IDC_BTN_APPLY           232

// Logging tab controls
#define IDC_LOG_STATUS          240
#define IDC_LOG_EDIT            241
#define IDC_BTN_LOG_CLEAR       242
#define IDC_BTN_LOG_COPY        243
#define IDC_LBL_LOG_VERBOSITY   244
#define IDC_LOG_VERBOSITY       245

// Tray menu commands
#define IDM_SETTINGS            301
#define IDM_SCAN_NOW            302
#define IDM_ABOUT               303
#define IDM_EXIT                304

// Timer for tray
#define IDT_TRAY_TIMER          400
#define IDT_LOG_POLL            401
#define IDT_SCAN_ANIM           402

// Custom window message: Logging tab → main window with scanning state
// wParam: 1 = scanning, 0 = idle
#define WM_SCAN_STATE           (WM_USER + 2)
