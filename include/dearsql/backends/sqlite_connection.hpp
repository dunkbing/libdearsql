#pragma once

#include "dearsql/database.hpp"

struct sqlite3;

namespace dearsql {

// SQLite combines connection + database in one file. database() is the shared
// handle; openDatabase() opens an independent one for a host's worker pool.
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
    DatabasePtr openDatabase(const std::string& name = "") override;

    // raw handle of the shared database(); nullptr if not open
    [[nodiscard]] sqlite3* handle() const;

private:
    ConnectionInfo info_;
    std::shared_ptr<void> handle_;
    DatabasePtr defaultDb_;
};

} // namespace dearsql
