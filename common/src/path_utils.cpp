#include "dirsize/path_utils.h"

#include <Windows.h>
#include <Winnetwk.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <mutex>
#include <optional>
#include <shared_mutex>

namespace dirsize {

namespace {

constexpr ULONGLONG kDriveMapCacheTtlMs = 30000;

struct DriveMapCacheEntry {
    bool initialized = false;
    bool isRemote = false;
    ULONGLONG timestampMs = 0;
    std::wstring uncPrefix;
};

std::shared_mutex g_driveMapMutex;
std::array<DriveMapCacheEntry, 26> g_driveMapCache;

static std::wstring StripLongPathPrefix(const std::wstring& input) {
    if (input.size() > 8 && input.substr(0, 8) == L"\\\\?\\UNC\\") {
        return L"\\\\" + input.substr(8);
    }
    if (input.size() > 4 && input.substr(0, 4) == L"\\\\?\\") {
        return input.substr(4);
    }
    return input;
}

static std::optional<std::wstring> GetMappedUncPrefix(wchar_t driveLetter) {
    if (!iswalpha(driveLetter)) {
        return std::nullopt;
    }

    wchar_t upper = static_cast<wchar_t>(towupper(driveLetter));
    size_t index = static_cast<size_t>(upper - L'A');
    ULONGLONG now = GetTickCount64();

    {
        std::shared_lock<std::shared_mutex> lock(g_driveMapMutex);
        const auto& cached = g_driveMapCache[index];
        if (cached.initialized && now - cached.timestampMs <= kDriveMapCacheTtlMs) {
            if (cached.isRemote && !cached.uncPrefix.empty()) {
                return cached.uncPrefix;
            }
            return std::nullopt;
        }
    }

    std::unique_lock<std::shared_mutex> lock(g_driveMapMutex);
    auto& cached = g_driveMapCache[index];
    now = GetTickCount64();
    if (cached.initialized && now - cached.timestampMs <= kDriveMapCacheTtlMs) {
        if (cached.isRemote && !cached.uncPrefix.empty()) {
            return cached.uncPrefix;
        }
        return std::nullopt;
    }

    cached.initialized = true;
    cached.isRemote = false;
    cached.timestampMs = now;
    cached.uncPrefix.clear();

    wchar_t root[] = { upper, L':', L'\\', L'\0' };
    if (GetDriveTypeW(root) != DRIVE_REMOTE) {
        return std::nullopt;
    }

    wchar_t localName[] = { upper, L':', L'\0' };
    std::wstring remote;
    wchar_t stackBuf[512] = {};
    DWORD size = _countof(stackBuf);
    DWORD err = WNetGetConnectionW(localName, stackBuf, &size);
    if (err == NO_ERROR) {
        remote = stackBuf;
    } else if (err == ERROR_MORE_DATA && size > 0) {
        std::wstring dynamicBuf(static_cast<size_t>(size), L'\0');
        err = WNetGetConnectionW(localName, &dynamicBuf[0], &size);
        if (err != NO_ERROR || dynamicBuf.empty()) {
            return std::nullopt;
        }
        remote = dynamicBuf.c_str();
    } else {
        return std::nullopt;
    }
    std::replace(remote.begin(), remote.end(), L'/', L'\\');
    while (remote.size() > 2 && remote.back() == L'\\') {
        remote.pop_back();
    }

    if (remote.size() >= 2 && remote[0] == L'\\' && remote[1] == L'\\') {
        cached.isRemote = true;
        cached.uncPrefix = remote;
        return cached.uncPrefix;
    }

    return std::nullopt;
}

} // namespace

std::wstring CanonicalizePath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    std::wstring normalized = StripLongPathPrefix(path);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');

    if (normalized.size() >= 2 && normalized[1] == L':' && iswalpha(normalized[0])) {
        if (auto unc = GetMappedUncPrefix(normalized[0]); unc.has_value()) {
            std::wstring tail = (normalized.size() > 2) ? normalized.substr(2) : L"";
            if (!tail.empty() && tail.front() != L'\\') {
                tail.insert(tail.begin(), L'\\');
            }
            normalized = *unc + tail;
        }
    }

    while (normalized.size() > 3 && normalized.back() == L'\\') {
        normalized.pop_back();
    }

    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::towlower);
    return normalized;
}

std::wstring CanonicalizePath(const wchar_t* path) {
    if (!path) {
        return {};
    }
    return CanonicalizePath(std::wstring(path));
}

std::wstring GetVolumeRootPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }

    std::wstring normalized = StripLongPathPrefix(path);
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');

    if (normalized.size() >= 2 && normalized[1] == L':' && iswalpha(normalized[0])) {
        wchar_t upper = static_cast<wchar_t>(towupper(normalized[0]));
        wchar_t root[] = { upper, L':', L'\\', L'\0' };
        return root;
    }

    if (normalized.size() >= 3 && normalized[0] == L'\\' && normalized[1] == L'\\') {
        size_t serverEnd = normalized.find(L'\\', 2);
        if (serverEnd == std::wstring::npos) {
            return {};
        }
        size_t shareEnd = normalized.find(L'\\', serverEnd + 1);
        if (shareEnd == std::wstring::npos) {
            return normalized + L'\\';
        }
        return normalized.substr(0, shareEnd + 1);
    }

    return {};
}

} // namespace dirsize
