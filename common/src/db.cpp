#include "dirsize/db.h"
#include "dirsize/path_utils.h"

#include <sqlite3.h>
#include <ShlObj.h>
#include <algorithm>
#include <filesystem>

namespace dirsize {

namespace {

// Convert a wide string to UTF-8 for SQLite
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

// Convert UTF-8 to wide string
std::wstring Utf8ToWide(const char* utf8, int len = -1) {
    if (!utf8 || (len == 0)) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8, len, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, result.data(), size);
    // With len == -1 the conversion includes the terminating NUL in the
    // output. Drop it — an embedded NUL survives canonicalization and
    // WideToUtf8, so paths read from the database and written back would
    // silently land under a distinct "path\0" key.
    if (len == -1) result.pop_back();
    return result;
}

} // namespace

Database::Database() = default;

Database::~Database() {
    Close();
}

std::wstring Database::GetDefaultPath() {
    // Per-user location. The scan engine runs in the user's session (hosted
    // by the tray app) and Explorer's shell extension runs as the same user,
    // so both sides can read/write here — including SQLite's WAL side files,
    // which was impossible for non-admin readers under ProgramData.
    wchar_t* localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        std::wstring path(localAppData);
        CoTaskMemFree(localAppData);
        path += L"\\DirSizeForExplorer\\dirsize.db";
        return path;
    }
    return L"C:\\ProgramData\\DirSizeForExplorer\\dirsize.db";
}

bool Database::Open(const std::wstring& dbPath, bool readOnly) {
    std::lock_guard lock(m_mutex);

    if (m_db) return false; // Already open

    // Ensure parent directory exists (only if read-write)
    if (!readOnly) {
        std::filesystem::path p(dbPath);
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::string utf8Path = WideToUtf8(dbPath);
    int flags = readOnly
        ? (SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX)
        : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);

    int rc = sqlite3_open_v2(utf8Path.c_str(), &m_db, flags, nullptr);
    if (rc != SQLITE_OK) {
        if (m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
        return false;
    }

    // Configure pragmas
    sqlite3_exec(m_db, "PRAGMA journal_mode = WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA synchronous = NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA cache_size = -4000;", nullptr, nullptr, nullptr);
    sqlite3_exec(m_db, "PRAGMA busy_timeout = 1000;", nullptr, nullptr, nullptr);
    // Paths are stored canonicalized (lowercase), so case-sensitive LIKE is
    // safe and lets prefix patterns use the primary-key index instead of a
    // full table scan (matters for RemoveByPrefix / GetChildDirs).
    sqlite3_exec(m_db, "PRAGMA case_sensitive_like = ON;", nullptr, nullptr, nullptr);

    if (!readOnly) {
        if (!CreateTables() || !MigrateSchema()) {
            // Note: cannot call Close() here — it would relock m_mutex
            // (non-recursive) and deadlock.
            sqlite3_close(m_db);
            m_db = nullptr;
            return false;
        }
    }

    if (!PrepareStatements()) {
        // Leave the object fully closed on failure so IsOpen() is accurate
        // and callers (e.g. the shell hook) can cheaply early-out.
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    return true;
}

void Database::Close() {
    std::lock_guard lock(m_mutex);
    FinalizeStatements();
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool Database::IsOpen() const {
    return m_db != nullptr;
}

bool Database::CreateTables() {
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS dir_sizes (
            path       TEXT PRIMARY KEY NOT NULL,
            total_size INTEGER NOT NULL,
            alloc_size INTEGER NOT NULL DEFAULT 0,
            file_count INTEGER NOT NULL DEFAULT 0,
            dir_count  INTEGER NOT NULL DEFAULT 0,
            scan_time  INTEGER NOT NULL,
            depth      INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS usn_bookmarks (
            volume     TEXT PRIMARY KEY NOT NULL,
            journal_id INTEGER NOT NULL,
            last_usn   INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS meta (
            key   TEXT PRIMARY KEY NOT NULL,
            value INTEGER NOT NULL
        );
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return rc == SQLITE_OK;
}

bool Database::MigrateSchema() {
    // Check current schema version
    sqlite3_stmt* stmt = nullptr;
    int version = 0;
    if (sqlite3_prepare_v2(m_db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }

    if (version < 1) {
        // Add alloc_size column
        char* errMsg = nullptr;
        int rc = sqlite3_exec(m_db,
            "ALTER TABLE dir_sizes ADD COLUMN alloc_size INTEGER NOT NULL DEFAULT 0;",
            nullptr, nullptr, &errMsg);
        if (errMsg) sqlite3_free(errMsg);
        // Ignore error if column already exists (rc == SQLITE_ERROR with "duplicate column")
        if (rc != SQLITE_OK && rc != SQLITE_ERROR) return false;

        sqlite3_exec(m_db, "PRAGMA user_version = 1;", nullptr, nullptr, nullptr);
    }

    if (version < 2) {
        // Versions ≤ 2.3.0 read paths back from SQLite with an embedded
        // terminating NUL (Utf8ToWide bug) and re-upserted them under those
        // NUL-suffixed keys, so ancestor updates created unreachable ghost
        // rows while the real rows went stale. Purge the ghosts; the stale
        // real rows self-heal through normal scanning.
        sqlite3_exec(m_db,
            "DELETE FROM dir_sizes WHERE instr(CAST(path AS BLOB), x'00') > 0;"
            "DELETE FROM usn_bookmarks WHERE instr(CAST(volume AS BLOB), x'00') > 0;",
            nullptr, nullptr, nullptr);

        sqlite3_exec(m_db, "PRAGMA user_version = 2;", nullptr, nullptr, nullptr);
    }

    return true;
}

bool Database::PrepareStatements() {
    if (!m_db) return false;

    auto prepare = [this](const char* sql, sqlite3_stmt** stmt) -> bool {
        return sqlite3_prepare_v2(m_db, sql, -1, stmt, nullptr) == SQLITE_OK;
    };

    bool ok = true;
    ok = ok && prepare("SELECT total_size FROM dir_sizes WHERE path = ?1;", &m_stmtGetSize);
    ok = ok && prepare("SELECT alloc_size FROM dir_sizes WHERE path = ?1;", &m_stmtGetAllocSize);
    ok = ok && prepare("SELECT path, total_size, alloc_size, file_count, dir_count, scan_time, depth "
                       "FROM dir_sizes WHERE path = ?1;", &m_stmtGetEntry);
    ok = ok && prepare("INSERT INTO dir_sizes (path, total_size, alloc_size, file_count, dir_count, scan_time, depth) "
                       "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
                       "ON CONFLICT(path) DO UPDATE SET "
                       "total_size=excluded.total_size, alloc_size=excluded.alloc_size, "
                       "file_count=excluded.file_count, dir_count=excluded.dir_count, "
                       "scan_time=excluded.scan_time, depth=excluded.depth;", &m_stmtUpsert);
    // Delete the directory itself (?1, exact) and its children (?2, escaped
    // LIKE pattern "prefix\%"). Two separate terms so that removing
    // "c:\a" can't also remove an unrelated sibling like "c:\ab".
    ok = ok && prepare("DELETE FROM dir_sizes WHERE path = ?1 "
                       "OR path LIKE ?2 ESCAPE '!';", &m_stmtRemovePrefix);
    ok = ok && prepare("SELECT path FROM dir_sizes WHERE path LIKE ?1 ESCAPE '!' "
                       "AND depth = ?2;", &m_stmtGetChildren);
    ok = ok && prepare("SELECT volume, journal_id, last_usn FROM usn_bookmarks WHERE volume = ?1;",
                       &m_stmtGetUsn);
    ok = ok && prepare("INSERT INTO usn_bookmarks (volume, journal_id, last_usn) VALUES (?1, ?2, ?3) "
                       "ON CONFLICT(volume) DO UPDATE SET journal_id=excluded.journal_id, "
                       "last_usn=excluded.last_usn;", &m_stmtUpsertUsn);

    if (!ok) {
        FinalizeStatements();
    }
    return ok;
}

void Database::FinalizeStatements() {
    auto finalize = [](sqlite3_stmt*& stmt) {
        if (stmt) {
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
    };
    finalize(m_stmtGetSize);
    finalize(m_stmtGetAllocSize);
    finalize(m_stmtGetEntry);
    finalize(m_stmtUpsert);
    finalize(m_stmtRemovePrefix);
    finalize(m_stmtGetChildren);
    finalize(m_stmtGetUsn);
    finalize(m_stmtUpsertUsn);
}

std::wstring Database::NormalizePath(const std::wstring& path) {
    return CanonicalizePath(path);
}

uint64_t Database::GetEntryCount() {
    std::lock_guard lock(m_mutex);
    if (!m_db) return 0;

    uint64_t count = 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM dir_sizes", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            count = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
        }
        sqlite3_finalize(stmt);
    }
    return count;
}

std::optional<uint64_t> Database::GetSize(const std::wstring& path) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtGetSize) return std::nullopt;

    std::wstring norm = NormalizePath(path);
    std::string utf8 = WideToUtf8(norm);

    sqlite3_reset(m_stmtGetSize);
    sqlite3_bind_text(m_stmtGetSize, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);

    std::optional<uint64_t> result;
    if (sqlite3_step(m_stmtGetSize) == SQLITE_ROW) {
        result = static_cast<uint64_t>(sqlite3_column_int64(m_stmtGetSize, 0));
    }
    sqlite3_reset(m_stmtGetSize);
    return result;
}

std::optional<uint64_t> Database::GetAllocSize(const std::wstring& path) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtGetAllocSize) return std::nullopt;

    std::wstring norm = NormalizePath(path);
    std::string utf8 = WideToUtf8(norm);

    sqlite3_reset(m_stmtGetAllocSize);
    sqlite3_bind_text(m_stmtGetAllocSize, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);

    std::optional<uint64_t> result;
    if (sqlite3_step(m_stmtGetAllocSize) == SQLITE_ROW) {
        result = static_cast<uint64_t>(sqlite3_column_int64(m_stmtGetAllocSize, 0));
    }
    sqlite3_reset(m_stmtGetAllocSize);
    return result;
}

std::optional<DirEntry> Database::GetEntry(const std::wstring& path) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtGetEntry) return std::nullopt;

    std::wstring norm = NormalizePath(path);
    std::string utf8 = WideToUtf8(norm);

    sqlite3_reset(m_stmtGetEntry);
    sqlite3_bind_text(m_stmtGetEntry, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);

    std::optional<DirEntry> result;
    if (sqlite3_step(m_stmtGetEntry) == SQLITE_ROW) {
        DirEntry entry;
        const char* p = reinterpret_cast<const char*>(sqlite3_column_text(m_stmtGetEntry, 0));
        entry.path = p ? Utf8ToWide(p) : L"";
        entry.totalSize = static_cast<uint64_t>(sqlite3_column_int64(m_stmtGetEntry, 1));
        entry.allocSize = static_cast<uint64_t>(sqlite3_column_int64(m_stmtGetEntry, 2));
        entry.fileCount = static_cast<uint64_t>(sqlite3_column_int64(m_stmtGetEntry, 3));
        entry.dirCount = static_cast<uint64_t>(sqlite3_column_int64(m_stmtGetEntry, 4));
        entry.scanTime = sqlite3_column_int64(m_stmtGetEntry, 5);
        entry.depth = sqlite3_column_int(m_stmtGetEntry, 6);
        result = std::move(entry);
    }
    sqlite3_reset(m_stmtGetEntry);
    return result;
}

bool Database::UpsertEntry(const DirEntry& entry) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtUpsert) return false;

    std::wstring norm = NormalizePath(entry.path);
    std::string utf8 = WideToUtf8(norm);

    sqlite3_reset(m_stmtUpsert);
    sqlite3_bind_text(m_stmtUpsert, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);
    sqlite3_bind_int64(m_stmtUpsert, 2, static_cast<sqlite3_int64>(entry.totalSize));
    sqlite3_bind_int64(m_stmtUpsert, 3, static_cast<sqlite3_int64>(entry.allocSize));
    sqlite3_bind_int64(m_stmtUpsert, 4, static_cast<sqlite3_int64>(entry.fileCount));
    sqlite3_bind_int64(m_stmtUpsert, 5, static_cast<sqlite3_int64>(entry.dirCount));
    sqlite3_bind_int64(m_stmtUpsert, 6, entry.scanTime);
    sqlite3_bind_int(m_stmtUpsert, 7, entry.depth);

    bool ok = sqlite3_step(m_stmtUpsert) == SQLITE_DONE;
    sqlite3_reset(m_stmtUpsert);
    return ok;
}

bool Database::UpsertEntries(const std::vector<DirEntry>& entries) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtUpsert || !m_db) return false;

    // IMMEDIATE grabs the write lock up front, avoiding a mid-transaction
    // lock upgrade that can fail with SQLITE_BUSY halfway through the batch.
    sqlite3_exec(m_db, "BEGIN IMMEDIATE TRANSACTION;", nullptr, nullptr, nullptr);

    for (const auto& entry : entries) {
        std::wstring norm = NormalizePath(entry.path);
        std::string utf8 = WideToUtf8(norm);

        sqlite3_reset(m_stmtUpsert);
        sqlite3_bind_text(m_stmtUpsert, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);
        sqlite3_bind_int64(m_stmtUpsert, 2, static_cast<sqlite3_int64>(entry.totalSize));
        sqlite3_bind_int64(m_stmtUpsert, 3, static_cast<sqlite3_int64>(entry.allocSize));
        sqlite3_bind_int64(m_stmtUpsert, 4, static_cast<sqlite3_int64>(entry.fileCount));
        sqlite3_bind_int64(m_stmtUpsert, 5, static_cast<sqlite3_int64>(entry.dirCount));
        sqlite3_bind_int64(m_stmtUpsert, 6, entry.scanTime);
        sqlite3_bind_int(m_stmtUpsert, 7, entry.depth);

        if (sqlite3_step(m_stmtUpsert) != SQLITE_DONE) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_reset(m_stmtUpsert);
            return false;
        }
    }

    sqlite3_reset(m_stmtUpsert);
    return sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool Database::RemoveByPrefix(const std::wstring& pathPrefix) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtRemovePrefix) return false;

    std::wstring norm = NormalizePath(pathPrefix);
    std::string utf8Exact = WideToUtf8(norm);

    // Escape SQL LIKE wildcards so paths containing '_' or '%' don't
    // match (and delete) unrelated rows. '!' is the ESCAPE character
    // declared in the prepared statement.
    std::wstring escaped;
    escaped.reserve(norm.size() + 8);
    for (wchar_t c : norm) {
        if (c == L'%' || c == L'_' || c == L'!') {
            escaped += L'!';
        }
        escaped += c;
    }
    // Children only: "prefix\<anything>". Drive roots ("c:\") already end
    // with the separator, so don't add a second one.
    if (norm.empty() || norm.back() != L'\\') {
        escaped += L'\\';
    }
    escaped += L'%';
    std::string utf8Like = WideToUtf8(escaped);

    sqlite3_reset(m_stmtRemovePrefix);
    sqlite3_bind_text(m_stmtRemovePrefix, 1, utf8Exact.c_str(),
                      static_cast<int>(utf8Exact.size()), SQLITE_STATIC);
    sqlite3_bind_text(m_stmtRemovePrefix, 2, utf8Like.c_str(),
                      static_cast<int>(utf8Like.size()), SQLITE_STATIC);

    bool ok = sqlite3_step(m_stmtRemovePrefix) == SQLITE_DONE;
    sqlite3_reset(m_stmtRemovePrefix);
    return ok;
}

std::vector<std::wstring> Database::GetChildDirs(const std::wstring& parentPath,
                                                 int parentDepth) {
    std::lock_guard lock(m_mutex);
    std::vector<std::wstring> result;
    if (!m_stmtGetChildren) return result;

    std::wstring norm = NormalizePath(parentPath);

    // Escaped LIKE pattern "parent\%" (children only)
    std::wstring pattern;
    pattern.reserve(norm.size() + 8);
    for (wchar_t c : norm) {
        if (c == L'%' || c == L'_' || c == L'!') pattern += L'!';
        pattern += c;
    }
    if (pattern.empty() || pattern.back() != L'\\') pattern += L'\\';
    pattern += L'%';
    std::string utf8 = WideToUtf8(pattern);

    sqlite3_reset(m_stmtGetChildren);
    sqlite3_bind_text(m_stmtGetChildren, 1, utf8.c_str(),
                      static_cast<int>(utf8.size()), SQLITE_STATIC);
    sqlite3_bind_int(m_stmtGetChildren, 2, parentDepth + 1);

    while (sqlite3_step(m_stmtGetChildren) == SQLITE_ROW) {
        const char* p = reinterpret_cast<const char*>(
            sqlite3_column_text(m_stmtGetChildren, 0));
        if (p) result.push_back(Utf8ToWide(p));
    }
    sqlite3_reset(m_stmtGetChildren);
    return result;
}

int64_t Database::GetMetaInt64(const char* key, int64_t defaultValue) {
    std::lock_guard lock(m_mutex);
    if (!m_db) return defaultValue;

    int64_t result = defaultValue;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "SELECT value FROM meta WHERE key = ?1;",
                           -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            result = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    return result;
}

bool Database::SetMetaInt64(const char* key, int64_t value) {
    std::lock_guard lock(m_mutex);
    if (!m_db) return false;

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "INSERT INTO meta (key, value) VALUES (?1, ?2) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, value);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

// Meta-table key for a root's "scan owed" flag ("pending_scan:<root>").
static std::string PendingScanKey(const std::wstring& normalizedRoot) {
    return "pending_scan:" + WideToUtf8(normalizedRoot);
}

bool Database::GetPendingScan(const std::wstring& root) {
    std::string key = PendingScanKey(NormalizePath(root));
    return GetMetaInt64(key.c_str(), 0) != 0;
}

bool Database::SetPendingScan(const std::wstring& root, bool pending) {
    std::string key = PendingScanKey(NormalizePath(root));
    if (pending) {
        return SetMetaInt64(key.c_str(), 1);
    }

    std::lock_guard lock(m_mutex);
    if (!m_db) return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, "DELETE FROM meta WHERE key = ?1;",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, key.c_str(), static_cast<int>(key.size()),
                      SQLITE_STATIC);
    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool Database::RemoveStaleUnder(const std::wstring& root,
                                int64_t scanStartEpochSeconds) {
    std::lock_guard lock(m_mutex);
    if (!m_db) return false;

    std::wstring norm = NormalizePath(root);
    if (norm.empty()) return false;

    // Escaped LIKE pattern "root\%" (strictly children — the root row
    // itself was just refreshed by the scan that triggered this purge).
    std::wstring pattern;
    pattern.reserve(norm.size() + 8);
    for (wchar_t c : norm) {
        if (c == L'%' || c == L'_' || c == L'!') pattern += L'!';
        pattern += c;
    }
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'%';
    std::string utf8 = WideToUtf8(pattern);

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db,
            "DELETE FROM dir_sizes WHERE path LIKE ?1 ESCAPE '!' "
            "AND scan_time < ?2;", -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, utf8.c_str(), static_cast<int>(utf8.size()),
                      SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, scanStartEpochSeconds);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

void Database::Vacuum() {
    std::lock_guard lock(m_mutex);
    if (!m_db) return;
    sqlite3_exec(m_db, "VACUUM;", nullptr, nullptr, nullptr);
}

bool Database::RemoveOutsideRoots(const std::vector<std::wstring>& roots) {
    std::lock_guard lock(m_mutex);
    if (!m_db || roots.empty()) return false;

    // Escape a normalized path for LIKE (ESCAPE '!') and build the
    // children pattern "root\%" (drive roots already end in '\').
    auto childPattern = [](const std::wstring& norm) {
        std::wstring pattern;
        pattern.reserve(norm.size() + 8);
        for (wchar_t c : norm) {
            if (c == L'%' || c == L'_' || c == L'!') pattern += L'!';
            pattern += c;
        }
        if (pattern.empty() || pattern.back() != L'\\') pattern += L'\\';
        pattern += L'%';
        return pattern;
    };

    // DELETE ... WHERE NOT ((path=?1 OR path LIKE ?2) OR (path=?3 ...) ...)
    std::string sql = "DELETE FROM dir_sizes WHERE NOT (";
    for (size_t i = 0; i < roots.size(); i++) {
        if (i) sql += " OR ";
        sql += "(path = ? OR path LIKE ? ESCAPE '!')";
    }
    sql += ");";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    // Bound strings must stay alive until step() completes
    std::vector<std::string> bound;
    bound.reserve(roots.size() * 2);
    int param = 1;
    for (const auto& root : roots) {
        std::wstring norm = NormalizePath(root);
        bound.push_back(WideToUtf8(norm));
        sqlite3_bind_text(stmt, param++, bound.back().c_str(),
                          static_cast<int>(bound.back().size()), SQLITE_STATIC);
        bound.push_back(WideToUtf8(childPattern(norm)));
        sqlite3_bind_text(stmt, param++, bound.back().c_str(),
                          static_cast<int>(bound.back().size()), SQLITE_STATIC);
    }

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    int removed = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);

    // Reclaim file space after a substantial prune (rare event — only
    // when the watched-dir list shrinks).
    if (ok && removed > 1000) {
        sqlite3_exec(m_db, "VACUUM;", nullptr, nullptr, nullptr);
    }
    return ok;
}

bool Database::RenamePrefix(const std::wstring& oldPrefix,
                            const std::wstring& newPrefix,
                            int depthDelta) {
    std::lock_guard lock(m_mutex);
    if (!m_db) return false;

    std::wstring oldNorm = NormalizePath(oldPrefix);
    std::wstring newNorm = NormalizePath(newPrefix);
    if (oldNorm.empty() || newNorm.empty() || oldNorm == newNorm) return false;

    // Escaped children pattern "old\%"
    std::wstring pattern;
    pattern.reserve(oldNorm.size() + 8);
    for (wchar_t c : oldNorm) {
        if (c == L'%' || c == L'_' || c == L'!') pattern += L'!';
        pattern += c;
    }
    if (pattern.back() != L'\\') pattern += L'\\';
    pattern += L'%';

    std::string oldUtf8 = WideToUtf8(oldNorm);
    std::string patUtf8 = WideToUtf8(pattern);
    std::string newUtf8 = WideToUtf8(newNorm);

    // OR REPLACE: if stale rows already exist under the new name they are
    // overwritten instead of tripping the primary-key constraint.
    const char* sql =
        "UPDATE OR REPLACE dir_sizes "
        "SET path = ?3 || substr(path, length(?1) + 1), depth = depth + ?4 "
        "WHERE path = ?1 OR path LIKE ?2 ESCAPE '!';";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, oldUtf8.c_str(), static_cast<int>(oldUtf8.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, patUtf8.c_str(), static_cast<int>(patUtf8.size()), SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, newUtf8.c_str(), static_cast<int>(newUtf8.size()), SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, depthDelta);

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::optional<UsnBookmark> Database::GetUsnBookmark(const std::wstring& volume) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtGetUsn) return std::nullopt;

    std::wstring norm = volume;
    std::transform(norm.begin(), norm.end(), norm.begin(), ::towlower);
    std::string utf8 = WideToUtf8(norm);

    sqlite3_reset(m_stmtGetUsn);
    sqlite3_bind_text(m_stmtGetUsn, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);

    std::optional<UsnBookmark> result;
    if (sqlite3_step(m_stmtGetUsn) == SQLITE_ROW) {
        UsnBookmark bm;
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(m_stmtGetUsn, 0));
        bm.volume = v ? Utf8ToWide(v) : L"";
        bm.journalId = static_cast<uint64_t>(sqlite3_column_int64(m_stmtGetUsn, 1));
        bm.lastUsn = sqlite3_column_int64(m_stmtGetUsn, 2);
        result = std::move(bm);
    }
    sqlite3_reset(m_stmtGetUsn);
    return result;
}

bool Database::UpsertUsnBookmark(const UsnBookmark& bookmark) {
    std::lock_guard lock(m_mutex);
    if (!m_stmtUpsertUsn) return false;

    std::wstring norm = bookmark.volume;
    std::transform(norm.begin(), norm.end(), norm.begin(), ::towlower);
    std::string utf8 = WideToUtf8(norm);

    sqlite3_reset(m_stmtUpsertUsn);
    sqlite3_bind_text(m_stmtUpsertUsn, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);
    sqlite3_bind_int64(m_stmtUpsertUsn, 2, static_cast<sqlite3_int64>(bookmark.journalId));
    sqlite3_bind_int64(m_stmtUpsertUsn, 3, bookmark.lastUsn);

    bool ok = sqlite3_step(m_stmtUpsertUsn) == SQLITE_DONE;
    sqlite3_reset(m_stmtUpsertUsn);
    return ok;
}

} // namespace dirsize
