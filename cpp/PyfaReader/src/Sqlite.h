#pragma once

// Thin RAII wrapper around the SQLite C API.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

class SqliteError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class Statement
{
public:
    Statement(sqlite3* db, const std::string& sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&& other) noexcept;

    // Parameters are 1-based, as in sqlite3_bind_*.
    Statement& bind(int index, int64_t value);
    Statement& bind(int index, double value);
    Statement& bind(int index, const std::string& value);
    Statement& bindNull(int index);

    // Advances to the next row. Returns false when there are no more rows.
    bool step();
    void reset();

    // Columns are 0-based, as in sqlite3_column_*.
    bool isNull(int col) const;
    int64_t getInt(int col) const;
    double getDouble(int col) const;
    std::string getText(int col) const;
    std::optional<int64_t> getOptInt(int col) const;
    std::optional<std::string> getOptText(int col) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database
{
public:
    // Opens read-only by default so pyfa's data can't be modified by accident
    // (pyfa may also have saveddata.db open at the same time).
    explicit Database(const std::filesystem::path& path, bool readOnly = true);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    Statement prepare(const std::string& sql) const;
    void exec(const std::string& sql) const;

    sqlite3* handle() const { return db_; }
    const std::filesystem::path& path() const { return path_; }

private:
    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
};
