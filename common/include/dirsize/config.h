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

struct Config {
    uint32_t scanIntervalMinutes = 30;
    DisplayFormat displayFormat = DisplayFormat::ExplorerDefault;
    bool autoScaleFoldersOnly = true;  // When AutoScale: true = folders only, false = files + folders
    SizeMetric sizeMetric = SizeMetric::LogicalSize;
    IOPriorityLevel ioPriority = IOPriorityLevel::Low;
    bool useChangeJournal = true;
    std::vector<std::wstring> watchedDirs;
};

// Registry key: HKLM\SOFTWARE\DirSizeForExplorer
inline constexpr wchar_t kRegistryKey[] = L"SOFTWARE\\DirSizeForExplorer";

// Read the full configuration from the registry.
// Returns defaults for any missing values.
Config LoadConfig();

// Write configuration to the registry.
// Requires elevation or appropriate ACL.
bool SaveConfig(const Config& config);

// Read a single DWORD value from the registry. Returns defaultVal if missing.
uint32_t ReadRegDword(const wchar_t* valueName, uint32_t defaultVal);

} // namespace dirsize
