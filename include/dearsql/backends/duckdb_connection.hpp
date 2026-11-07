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

// how a csv file is laid out, for reading it with DuckDB
struct CsvFormat {
    std::string delimiter = ",";
    char quote = '"';
    bool header = true;
    std::string encoding = "utf-8"; // utf-8, utf-16 or latin-1
};

// 0-based data rows (header excluded) of a csv file where `where`, a DuckDB SQL
// condition over the file's columns, holds. reads the file as saved, on its own
// in-memory database; throws Error on an unreadable file or a bad condition
std::vector<std::size_t> csvMatchingRows(const std::string& path, const std::string& where,
                                         const CsvFormat& format = {});

} // namespace dearsql
