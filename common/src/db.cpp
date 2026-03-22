#include "dirsize/db.h"

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
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, len, result.data(), size);
    return result;
}

} // namespace

Database::Database() = default;

Database::~Database() {
    Close();
}

std::wstring Database::GetDefaultPath() {
    wchar_t* programData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
        std::wstring path(programData);
        CoTaskMemFree(programData);
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

    if (!readOnly) {
        if (!CreateTables() || !MigrateSchema()) {
            Close();
            return false;
        }
    }

    return PrepareStatements();
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
    ok = ok && prepare("DELETE FROM dir_sizes WHERE path LIKE ?1;", &m_stmtRemovePrefix);
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
    finalize(m_stmtGetUsn);
    finalize(m_stmtUpsertUsn);
}

std::wstring Database::NormalizePath(const std::wstring& path) {
    std::wstring normalized = path;
    // Convert forward slashes to backslashes
    std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
    // Remove trailing backslash (unless it's a root like "C:\")
    while (normalized.size() > 3 && normalized.back() == L'\\') {
        normalized.pop_back();
    }
    // Lowercase for consistent lookups
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::towlower);
    return normalized;
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

    sqlite3_exec(m_db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

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
    norm += L'%'; // SQL LIKE pattern
    std::string utf8 = WideToUtf8(norm);

    sqlite3_reset(m_stmtRemovePrefix);
    sqlite3_bind_text(m_stmtRemovePrefix, 1, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_STATIC);

    bool ok = sqlite3_step(m_stmtRemovePrefix) == SQLITE_DONE;
    sqlite3_reset(m_stmtRemovePrefix);
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
