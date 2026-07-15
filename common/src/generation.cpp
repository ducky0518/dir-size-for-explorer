#include "dirsize/generation.h"

#include <Windows.h>

namespace dirsize {

namespace {

// Create-or-open the shared counter. "Local\" scopes it to the current
// session, matching the per-session engine + Explorer pairing. The mapping
// handle is intentionally kept open for the life of the process.
volatile LONG* GetGenerationPtr() {
    static volatile LONG* ptr = []() -> volatile LONG* {
        HANDLE hMapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(LONG),
            L"Local\\DirSizeForExplorer_Generation");
        if (!hMapping) return nullptr;
        void* view = MapViewOfFile(hMapping, FILE_MAP_ALL_ACCESS,
                                   0, 0, sizeof(LONG));
        return static_cast<volatile LONG*>(view);
    }();
    return ptr;
}

} // namespace

uint32_t GetSizeDataGeneration() {
    volatile LONG* p = GetGenerationPtr();
    return p ? static_cast<uint32_t>(*p) : 0;
}

void BumpSizeDataGeneration() {
    volatile LONG* p = GetGenerationPtr();
    if (p) InterlockedIncrement(const_cast<LONG*>(p));
}

} // namespace dirsize
