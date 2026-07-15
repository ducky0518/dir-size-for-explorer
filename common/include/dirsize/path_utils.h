#pragma once

#include <string>

namespace dirsize {

// Canonical path form used across scanner + shell lookup:
// - backslashes
// - long-path prefix removed (\\?\ and \\?\UNC\)
// - mapped drive letters converted to UNC when possible
// - no trailing slash (except drive root like C:\)
// - lowercase
std::wstring CanonicalizePath(const std::wstring& path);
std::wstring CanonicalizePath(const wchar_t* path);

// Returns the volume root for local/UNC paths:
// - C:\foo\bar -> C:\
// - \\server\share\foo -> \\server\share\
// Returns empty string when a root cannot be determined.
std::wstring GetVolumeRootPath(const std::wstring& path);

} // namespace dirsize
