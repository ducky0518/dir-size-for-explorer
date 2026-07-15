#include "dirsize/config.h"

#include <Windows.h>

namespace dirsize {

// Settings are per user: HKCU is authoritative (written by the settings
// dialog, no elevation needed), HKLM holds read-only machine defaults
// installed by the MSI. Reading HKCU first also migrates old installs
// transparently — values saved to HKLM by previous versions keep working
// as fallbacks until the first per-user save. HKLM must never be
// user-writable: one standard user could otherwise repoint every other
// user's scan engine (e.g. at a hostile UNC path).
uint32_t ReadRegDword(const wchar_t* valueName, uint32_t defaultVal) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryKey, valueName,
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value;
    }
    size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kRegistryKey, valueName,
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value;
    }
    return defaultVal;
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
    config.trackRenames = ReadRegDword(L"TrackRenames", 1) != 0;

    // Read a REG_MULTI_SZ value into a string list. Per-user value wins;
    // the machine-wide value is only a fallback (defaults / pre-2.4
    // installs), not merged.
    auto readMultiSz = [](const wchar_t* valueName,
                          std::vector<std::wstring>& out) {
        auto readFrom = [&](HKEY root) -> bool {
            DWORD dataSize = 0;
            LSTATUS status = RegGetValueW(
                root, kRegistryKey, valueName,
                RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &dataSize);
            if (status != ERROR_SUCCESS || dataSize == 0) return false;

            std::vector<wchar_t> buffer(dataSize / sizeof(wchar_t));
            status = RegGetValueW(
                root, kRegistryKey, valueName,
                RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &dataSize);
            if (status != ERROR_SUCCESS) return false;

            const wchar_t* p = buffer.data();
            while (*p) {
                out.emplace_back(p);
                p += wcslen(p) + 1;
            }
            return true;
        };
        if (!readFrom(HKEY_CURRENT_USER)) {
            readFrom(HKEY_LOCAL_MACHINE);
        }
    };

    // Watched dirs are stored as "path" or "path|intervalMinutes"
    // ('|' is invalid in Windows paths, so it's a safe separator).
    std::vector<std::wstring> rawWatched;
    readMultiSz(L"WatchedDirs", rawWatched);
    for (const auto& raw : rawWatched) {
        WatchedDir wd;
        wd.scanIntervalMinutes = config.scanIntervalMinutes;
        size_t sep = raw.find_last_of(L'|');
        if (sep != std::wstring::npos && sep + 1 < raw.size()) {
            uint32_t minutes = static_cast<uint32_t>(
                wcstoul(raw.c_str() + sep + 1, nullptr, 10));
            if (minutes >= 1 && minutes <= 1440) {
                wd.path = raw.substr(0, sep);
                wd.scanIntervalMinutes = minutes;
            } else {
                wd.path = raw;
            }
        } else {
            wd.path = raw;
        }
        if (!wd.path.empty()) config.watchedDirs.push_back(std::move(wd));
    }

    readMultiSz(L"ExcludedDirs", config.excludedDirs);

    // Default watched dirs if none configured
    if (config.watchedDirs.empty()) {
        WatchedDir wd;
        wd.path = L"C:\\";
        wd.scanIntervalMinutes = config.scanIntervalMinutes;
        config.watchedDirs.push_back(std::move(wd));
    }

    return config;
}

bool SaveConfig(const Config& config) {
    // Always saved per user (HKCU) — needs no elevation and can't affect
    // other users' scan engines.
    HKEY hKey = nullptr;
    LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRegistryKey, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);

    if (status != ERROR_SUCCESS) return false;

    bool ok = true;
    ok = ok && WriteRegDword(hKey, L"ScanIntervalMinutes", config.scanIntervalMinutes);
    ok = ok && WriteRegDword(hKey, L"DisplayFormat", static_cast<uint32_t>(config.displayFormat));
    ok = ok && WriteRegDword(hKey, L"AutoScaleFoldersOnly", config.autoScaleFoldersOnly ? 1 : 0);
    ok = ok && WriteRegDword(hKey, L"SizeMetric", static_cast<uint32_t>(config.sizeMetric));
    ok = ok && WriteRegDword(hKey, L"IOPriority", static_cast<uint32_t>(config.ioPriority));
    ok = ok && WriteRegDword(hKey, L"UseChangeJournal", config.useChangeJournal ? 1 : 0);
    ok = ok && WriteRegDword(hKey, L"TrackRenames", config.trackRenames ? 1 : 0);

    // Write a string list as REG_MULTI_SZ
    auto writeMultiSz = [&](const wchar_t* valueName,
                            const std::vector<std::wstring>& dirs) -> bool {
        std::wstring multiSz;
        for (const auto& dir : dirs) {
            multiSz += dir;
            multiSz += L'\0';
        }
        multiSz += L'\0'; // Double null terminator

        DWORD bytes = static_cast<DWORD>(multiSz.size() * sizeof(wchar_t));
        return RegSetValueExW(hKey, valueName, 0, REG_MULTI_SZ,
                              reinterpret_cast<const BYTE*>(multiSz.data()),
                              bytes) == ERROR_SUCCESS;
    };

    std::vector<std::wstring> rawWatched;
    for (const auto& wd : config.watchedDirs) {
        rawWatched.push_back(wd.path + L"|" +
                             std::to_wstring(wd.scanIntervalMinutes));
    }
    ok = ok && writeMultiSz(L"WatchedDirs", rawWatched);
    ok = ok && writeMultiSz(L"ExcludedDirs", config.excludedDirs);

    RegCloseKey(hKey);
    return ok;
}

} // namespace dirsize
