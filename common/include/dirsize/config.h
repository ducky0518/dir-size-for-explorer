#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dirsize {

enum class DisplayFormat : uint32_t {
    ExplorerDefault = 0,  // Show sizes in KB (Windows default)
    AutoScale = 1,        // Auto-scale size labels (KB/MB/GB)
};

enum class SizeMetric : uint32_t {
    LogicalSize = 0,     // "Size" — sum of file logical sizes
    AllocationSize = 1,  // "Size on disk" — sum of cluster-rounded sizes
};

enum class IOPriorityLevel : uint32_t {
    VeryLow = 0,
    Low = 1,
    Normal = 2,
};

// A watched root with its own full-scan interval. Interval scans act as a
// consistency backstop; once real-time change watching is confirmed for a
// root, its interval scans are suspended automatically.
struct WatchedDir {
    std::wstring path;
    uint32_t scanIntervalMinutes = 30;
};

struct Config {
    uint32_t scanIntervalMinutes = 30; // Default interval for new watched dirs
    DisplayFormat displayFormat = DisplayFormat::ExplorerDefault;
    bool autoScaleFoldersOnly = true;  // When AutoScale: true = folders only, false = files + folders
    SizeMetric sizeMetric = SizeMetric::LogicalSize;
    IOPriorityLevel ioPriority = IOPriorityLevel::Low;
    bool useChangeJournal = true;
    // Follow watched folders when they are renamed or moved: the watched-dir
    // config entry and all cached sizes are updated to the new path.
    bool trackRenames = true;
    std::vector<WatchedDir> watchedDirs;
    // Directories to skip entirely: not scanned, not counted in any
    // ancestor's total, and their cached entries are purged.
    std::vector<std::wstring> excludedDirs;
};

// Registry subkey used under both hives:
//   HKCU\SOFTWARE\DirSizeForExplorer — per-user settings (authoritative,
//       written by the settings dialog without elevation)
//   HKLM\SOFTWARE\DirSizeForExplorer — read-only machine defaults
//       installed by the MSI (also serves as the fallback for settings
//       saved there by versions before 2.4)
inline constexpr wchar_t kRegistryKey[] = L"SOFTWARE\\DirSizeForExplorer";

// Read the full configuration from the registry (HKCU first, HKLM
// defaults as fallback). Returns defaults for any missing values.
Config LoadConfig();

// Write configuration to HKCU. No elevation needed.
bool SaveConfig(const Config& config);

// Read a single DWORD value (HKCU, then HKLM). Returns defaultVal if missing.
uint32_t ReadRegDword(const wchar_t* valueName, uint32_t defaultVal);

} // namespace dirsize
