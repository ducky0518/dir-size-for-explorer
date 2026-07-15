#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <mutex>

struct sqlite3;
struct sqlite3_stmt;

namespace dirsize {

struct DirEntry {
    std::wstring path;
    uint64_t totalSize = 0;
    uint64_t allocSize = 0; // "Size on disk" — cluster-rounded
    uint64_t fileCount = 0;
    uint64_t dirCount = 0;
    int64_t scanTime = 0;   // Unix timestamp (seconds)
    int depth = 0;
};

struct UsnBookmark {
    std::wstring volume;    // e.g., L"C:"
    uint64_t journalId = 0;
    int64_t lastUsn = 0;
};

// Thread-safe SQLite database wrapper.
// The service opens in read-write mode; the shell extension opens read-only.
class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Open the database. If readOnly is true, opens with SQLITE_OPEN_READONLY.
    // Creates tables if they don't exist (only in read-write mode).
    bool Open(const std::wstring& dbPath, bool readOnly = false);
    void Close();
    bool IsOpen() const;

    // Returns the default database path: %ProgramData%\DirSizeForExplorer\dirsize.db
    static std::wstring GetDefaultPath();

    // --- Size queries (used by shell extension) ---

    // Get total number of entries in the database.
    uint64_t GetEntryCount();

    // Look up the cached total size for a directory path.
    // Returns nullopt if not found.
    std::optional<uint64_t> GetSize(const std::wstring& path);

    // Look up the cached allocation size ("size on disk") for a directory path.
    std::optional<uint64_t> GetAllocSize(const std::wstring& path);

    // Look up the full entry for a directory.
    std::optional<DirEntry> GetEntry(const std::wstring& path);

    // --- Size writes (used by the service) ---

    // Insert or update a directory's size data.
    bool UpsertEntry(const DirEntry& entry);

    // Batch upsert inside a single transaction for performance.
    bool UpsertEntries(const std::vector<DirEntry>& entries);

    // Remove entries whose paths start with the given prefix.
    bool RemoveByPrefix(const std::wstring& pathPrefix);

    // Rebuild the database file to reclaim free pages (e.g. after purging
    // a large excluded subtree). Blocking; call only on rare events.
    void Vacuum();

    // Simple key/value metadata (e.g. the engine liveness heartbeat used
    // to size the blind window across restarts).
    int64_t GetMetaInt64(const char* key, int64_t defaultValue);
    bool SetMetaInt64(const char* key, int64_t value);

    // Per-root "scan owed" flag, persisted across restarts. Set when a
    // root scan starts (or a reconciliation is requested by a watcher) and
    // cleared only when a scan of that root COMPLETES. At startup a set
    // flag forces a reconciliation scan regardless of how fresh the
    // liveness heartbeat makes the cache look — an interrupted scan must
    // not be masked by a clean shutdown.
    bool GetPendingScan(const std::wstring& root);
    bool SetPendingScan(const std::wstring& root, bool pending);

    // Remove every entry STRICTLY UNDER `root` whose scan_time predates
    // scanStartEpochSeconds. Called after a fully completed root scan so
    // subtrees deleted from disk while the engine wasn't watching don't
    // survive in the cache forever (full scans only ever upserted before).
    bool RemoveStaleUnder(const std::wstring& root,
                          int64_t scanStartEpochSeconds);

    // Remove every entry that does NOT fall under one of the given roots.
    // Called when the watched-directory list changes so trees that are no
    // longer watched don't accumulate forever. Runs VACUUM afterwards if
    // a substantial number of rows were removed. No-op if roots is empty
    // (safety: never wipe the whole database by accident).
    bool RemoveOutsideRoots(const std::vector<std::wstring>& roots);

    // List the immediate child directories of `parentPath` that exist in
    // the database (children have depth == parentDepth + 1 and path prefix
    // "parent\"). Used by shallow rescans to detect deleted subtrees.
    std::vector<std::wstring> GetChildDirs(const std::wstring& parentPath,
                                           int parentDepth);

    // Rename a directory and its whole cached subtree in place: every path
    // beginning with oldPrefix is rewritten to begin with newPrefix, and
    // depth is adjusted by depthDelta (non-zero when the directory moved to
    // a different tree level). Preserves cached sizes across renames/moves
    // so Explorer keeps showing them instantly.
    bool RenamePrefix(const std::wstring& oldPrefix,
                      const std::wstring& newPrefix,
                      int depthDelta);

    // --- USN bookmarks ---

    std::optional<UsnBookmark> GetUsnBookmark(const std::wstring& volume);
    bool UpsertUsnBookmark(const UsnBookmark& bookmark);

private:
    // Normalize a path for consistent storage/lookup:
    // canonicalized form (mapped drives -> UNC when available,
    // long-path prefix removed, lowercase, backslashes, no trailing slash).
    static std::wstring NormalizePath(const std::wstring& path);

    bool CreateTables();
    bool MigrateSchema();
    bool PrepareStatements();
    void FinalizeStatements();

    sqlite3* m_db = nullptr;
    std::mutex m_mutex;

    // Prepared statements (lazily created)
    sqlite3_stmt* m_stmtGetSize = nullptr;
    sqlite3_stmt* m_stmtGetAllocSize = nullptr;
    sqlite3_stmt* m_stmtGetEntry = nullptr;
    sqlite3_stmt* m_stmtUpsert = nullptr;
    sqlite3_stmt* m_stmtRemovePrefix = nullptr;
    sqlite3_stmt* m_stmtGetChildren = nullptr;
    sqlite3_stmt* m_stmtGetUsn = nullptr;
    sqlite3_stmt* m_stmtUpsertUsn = nullptr;
};

} // namespace dirsize
