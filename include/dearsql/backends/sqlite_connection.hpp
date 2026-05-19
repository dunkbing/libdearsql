#pragma once

#include "dearsql/database.hpp"

struct sqlite3;

namespace dearsql {

// SQLite combines connection + database + schema in one object. The
// connection owns the sqlite3* and serves as the only database (file).
class SQLiteConnection final : public IConnection {
public:
    explicit SQLiteConnection(const ConnectionInfo& info);
    ~SQLiteConnection() override;

    SQLiteConnection(const SQLiteConnection&) = delete;
    SQLiteConnection& operator=(const SQLiteConnection&) = delete;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::SQLITE;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;

    // raw handle for advanced use; returns nullptr if not open.
    [[nodiscard]] sqlite3* handle() const {
        return db_;
    }

private:
    ConnectionInfo info_;
    sqlite3* db_ = nullptr;
    DatabasePtr defaultDb_;
};

} // namespace dearsql
