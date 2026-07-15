#pragma once

#include <cstdint>

namespace dirsize {

// A session-local, cross-process "size data generation" counter backed by
// named shared memory.
//
// The scan engine (tray process) bumps it whenever it writes fresh sizes
// to the database; the shell extension (Explorer process) stores the
// generation in each in-memory cache entry and treats entries from an
// older generation as stale. Combined with SHChangeNotify, this makes
// Explorer show updated sizes immediately after a rescan instead of
// serving up-to-60s-old cached values.

// Current generation (0 if the shared mapping is unavailable).
uint32_t GetSizeDataGeneration();

// Increment the generation (called by the engine after DB writes).
void BumpSizeDataGeneration();

} // namespace dirsize
