#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

// one duckdb database per file; database() shares a connection, openDatabase()
// opens another connection on the same database (cheap, safe to run in parallel)
class DuckDBConnection final : public IConnection {
public:
    explicit DuckDBConnection(const ConnectionInfo& info);
    ~DuckDBConnection() override;

    DuckDBConnection(const DuckDBConnection&) = delete;
    DuckDBConnection& operator=(const DuckDBConnection&) = delete;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::DUCKDB;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;
    DatabasePtr openDatabase(const std::string& name = "") override;

private:
    ConnectionInfo info_;
    std::shared_ptr<void> db_; // duckdb_database, kept alive by every handle
    DatabasePtr defaultDb_;
};

} // namespace dearsql
