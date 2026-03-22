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

    // --- USN bookmarks ---

    std::optional<UsnBookmark> GetUsnBookmark(const std::wstring& volume);
    bool UpsertUsnBookmark(const UsnBookmark& bookmark);

private:
    // Normalize a path for consistent storage/lookup:
    // lowercase, backslashes, no trailing backslash.
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
    sqlite3_stmt* m_stmtGetUsn = nullptr;
    sqlite3_stmt* m_stmtUpsertUsn = nullptr;
};

} // namespace dirsize
