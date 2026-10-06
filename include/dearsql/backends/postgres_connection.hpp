#pragma once

#include "dearsql/database.hpp"
#include <memory>
#include <mutex>

struct pg_conn;   // PGconn from <libpq-fe.h>, kept out of the public header
struct pg_cancel; // PGcancel

namespace dearsql {

class PostgresSchema;

// One database handle over one PGconn. `database()` hands out a cached one per
// name; `openDatabase()` a fresh one, so a host can pool them and run queries in
// parallel. Schemas (`schemas()`, `schema(name)`) share this handle's PGconn.
class PostgresDatabase final : public IDatabase,
                               public std::enable_shared_from_this<PostgresDatabase> {
public:
    PostgresDatabase(ConnectionInfo info, std::string dbName);
    ~PostgresDatabase() override;

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return info_.type;
    }

    // connect now instead of on first use
    Status open();
    // false once libpq reports the session broken
    bool alive() override;
    // PQcancel; safe from any thread while execute() runs
    void cancel() override;
    // raw libpq handle, null until open; hold mutex() while using it
    [[nodiscard]] pg_conn* handle() const {
        return conn_;
    }
    std::mutex& mutex() {
        return mu_;
    }

    // every user schema (pg_catalog, information_schema, pg_toast and temp
    // schemas excluded)
    std::vector<DatabasePtr> schemas() override;
    // no catalog query; the handle sets search_path on this connection per call
    DatabasePtr schema(const std::string& schemaName) override;

    // catalog listings across all schemas
    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<Table> materializedViews() override;
    // "schema.sequence"
    std::vector<std::string> sequences() override;
    std::vector<Routine> routines() override;
    // "schema.table", or a table in current_schema()
    Table describeTable(const std::string& tableName) override;

    // phaseTimings: network latency, execution, data download, data parse
    QueryResult execute(const std::string& sql, int rowLimit = 1000) override;

private:
    friend class PostgresSchema;

    void ensureConn(); // caller holds mu_; throws
    QueryResult run(const std::string& sql, int rowLimit, bool timed);
    // runs sql with unqualified names resolving in schema ("" = the server default),
    // switching search_path only when the connection is on a different one
    QueryResult executeIn(const std::string& schema, const std::string& sql, int rowLimit);
    std::mutex pathMu_;
    std::string searchPath_;
    // rows of the first result; throws on failure
    std::vector<std::vector<std::string>> rows(const std::string& sql);
    std::vector<std::string> schemaNames();

    std::vector<Table> tablesIn(const std::string& schema);
    std::vector<Table> viewsIn(const std::string& schema);
    std::vector<Table> materializedViewsIn(const std::string& schema);
    std::vector<std::string> sequencesIn(const std::string& schema);
    std::vector<Routine> routinesIn(const std::string& schema);
    Table describeIn(const std::string& schema, const std::string& tableName);

    ConnectionInfo info_;
    std::string name_;
    pg_conn* conn_ = nullptr;
    std::mutex mu_;
    pg_cancel* cancel_ = nullptr;
    std::mutex cancelMu_;
};

// PostgreSQL / Redshift backend.
class PostgresConnection final : public IConnection {
public:
    explicit PostgresConnection(const ConnectionInfo& info);
    ~PostgresConnection() override;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return info_.type;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;
    DatabasePtr openDatabase(const std::string& name = "") override;

    // a comment that fails to apply still reports success, with a message
    Status createDatabase(const CreateDatabaseOptions& opts) override;
    // dropping the connected database goes through the maintenance database
    // (postgres, or dev on Redshift), which becomes the default from then on
    Status dropDatabase(const std::string& name) override;
    Status renameDatabase(const std::string& oldName, const std::string& newName) override;

private:
    ConnectionInfo info_;
    void* impl_ = nullptr;
};

} // namespace dearsql
