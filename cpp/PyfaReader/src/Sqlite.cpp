#include "Sqlite.h"

#include "sqlite3.h"

#include <utility>

namespace
{
[[noreturn]] void throwError(sqlite3* db, const std::string& what)
{
    throw SqliteError(what + ": " + (db ? sqlite3_errmsg(db) : "unknown error"));
}
}

// ---------------------------------------------------------------- Statement

Statement::Statement(sqlite3* db, const std::string& sql) : db_(db)
{
    if (sqlite3_prepare_v2(db_, sql.c_str(), static_cast<int>(sql.size()), &stmt_, nullptr) != SQLITE_OK)
        throwError(db_, "prepare failed for \"" + sql + "\"");
}

Statement::~Statement()
{
    sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept
    : db_(std::exchange(other.db_, nullptr)), stmt_(std::exchange(other.stmt_, nullptr))
{
}

Statement& Statement::operator=(Statement&& other) noexcept
{
    if (this != &other)
    {
        sqlite3_finalize(stmt_);
        db_ = std::exchange(other.db_, nullptr);
        stmt_ = std::exchange(other.stmt_, nullptr);
    }
    return *this;
}

Statement& Statement::bind(int index, int64_t value)
{
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK)
        throwError(db_, "bind failed");
    return *this;
}

Statement& Statement::bind(int index, double value)
{
    if (sqlite3_bind_double(stmt_, index, value) != SQLITE_OK)
        throwError(db_, "bind failed");
    return *this;
}

Statement& Statement::bind(int index, const std::string& value)
{
    if (sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throwError(db_, "bind failed");
    return *this;
}

Statement& Statement::bindNull(int index)
{
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK)
        throwError(db_, "bind failed");
    return *this;
}

bool Statement::step()
{
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    throwError(db_, "step failed");
}

void Statement::reset()
{
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
}

bool Statement::isNull(int col) const
{
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

int64_t Statement::getInt(int col) const
{
    return sqlite3_column_int64(stmt_, col);
}

double Statement::getDouble(int col) const
{
    return sqlite3_column_double(stmt_, col);
}

std::string Statement::getText(int col) const
{
    auto text = reinterpret_cast<const char*>(sqlite3_column_text(stmt_, col));
    if (!text)
        return {};
    return std::string(text, static_cast<size_t>(sqlite3_column_bytes(stmt_, col)));
}

std::optional<int64_t> Statement::getOptInt(int col) const
{
    if (isNull(col))
        return std::nullopt;
    return getInt(col);
}

std::optional<std::string> Statement::getOptText(int col) const
{
    if (isNull(col))
        return std::nullopt;
    return getText(col);
}

// ----------------------------------------------------------------- Database

Database::Database(const std::filesystem::path& path, bool readOnly) : path_(path)
{
    int flags = readOnly ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    // sqlite3_open_v2 expects UTF-8 on all platforms.
    auto u8 = path.u8string();
    std::string utf8(u8.begin(), u8.end());
    if (sqlite3_open_v2(utf8.c_str(), &db_, flags, nullptr) != SQLITE_OK)
    {
        std::string msg = "cannot open " + utf8 + ": " + (db_ ? sqlite3_errmsg(db_) : "out of memory");
        sqlite3_close(db_);
        throw SqliteError(msg);
    }
    // Wait a bit instead of failing immediately if pyfa is writing.
    sqlite3_busy_timeout(db_, 2000);
}

Database::~Database()
{
    sqlite3_close(db_);
}

Statement Database::prepare(const std::string& sql) const
{
    return Statement(db_, sql);
}

void Database::exec(const std::string& sql) const
{
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
    {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw SqliteError("exec failed: " + msg);
    }
}
