#include "dirsize/config.h"

#include <Windows.h>

namespace dirsize {

uint32_t ReadRegDword(const wchar_t* valueName, uint32_t defaultVal) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE, kRegistryKey, valueName,
        RRF_RT_REG_DWORD, nullptr, &value, &size);
    return (status == ERROR_SUCCESS) ? value : defaultVal;
}

static bool WriteRegDword(HKEY hKey, const wchar_t* valueName, uint32_t value) {
    return RegSetValueExW(hKey, valueName, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
}

Config LoadConfig() {
    Config config;

    config.scanIntervalMinutes = ReadRegDword(L"ScanIntervalMinutes", 30);
    config.displayFormat = static_cast<DisplayFormat>(ReadRegDword(L"DisplayFormat", 0));
    config.autoScaleFoldersOnly = ReadRegDword(L"AutoScaleFoldersOnly", 1) != 0;
    config.sizeMetric = static_cast<SizeMetric>(ReadRegDword(L"SizeMetric", 0));
    config.ioPriority = static_cast<IOPriorityLevel>(ReadRegDword(L"IOPriority", 1));
    config.useChangeJournal = ReadRegDword(L"UseChangeJournal", 1) != 0;

    // Read watched directories (REG_MULTI_SZ)
    DWORD dataSize = 0;
    LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE, kRegistryKey, L"WatchedDirs",
        RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &dataSize);

    if (status == ERROR_SUCCESS && dataSize > 0) {
        std::vector<wchar_t> buffer(dataSize / sizeof(wchar_t));
        status = RegGetValueW(
            HKEY_LOCAL_MACHINE, kRegistryKey, L"WatchedDirs",
            RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &dataSize);

        if (status == ERROR_SUCCESS) {
            const wchar_t* p = buffer.data();
            while (*p) {
                config.watchedDirs.emplace_back(p);
                p += wcslen(p) + 1;
            }
        }
    }

    // Default watched dirs if none configured
    if (config.watchedDirs.empty()) {
        config.watchedDirs.push_back(L"C:\\");
    }

    return config;
}

bool SaveConfig(const Config& config) {
    HKEY hKey = nullptr;
    LSTATUS status = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, kRegistryKey, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);

    if (status != ERROR_SUCCESS) return false;

    bool ok = true;
    ok = ok && WriteRegDword(hKey, L"ScanIntervalMinutes", config.scanIntervalMinutes);
    ok = ok && WriteRegDword(hKey, L"DisplayFormat", static_cast<uint32_t>(config.displayFormat));
    ok = ok && WriteRegDword(hKey, L"AutoScaleFoldersOnly", config.autoScaleFoldersOnly ? 1 : 0);
    ok = ok && WriteRegDword(hKey, L"SizeMetric", static_cast<uint32_t>(config.sizeMetric));
    ok = ok && WriteRegDword(hKey, L"IOPriority", static_cast<uint32_t>(config.ioPriority));
    ok = ok && WriteRegDword(hKey, L"UseChangeJournal", config.useChangeJournal ? 1 : 0);

    // Write watched directories as REG_MULTI_SZ
    std::wstring multiSz;
    for (const auto& dir : config.watchedDirs) {
        multiSz += dir;
        multiSz += L'\0';
    }
    multiSz += L'\0'; // Double null terminator

    DWORD multiSzBytes = static_cast<DWORD>(multiSz.size() * sizeof(wchar_t));
    ok = ok && (RegSetValueExW(hKey, L"WatchedDirs", 0, REG_MULTI_SZ,
                               reinterpret_cast<const BYTE*>(multiSz.data()),
                               multiSzBytes) == ERROR_SUCCESS);

    RegCloseKey(hKey);
    return ok;
}

} // namespace dirsize
