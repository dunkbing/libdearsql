#pragma once

#include "connection_info.hpp"
#include "query_result.hpp"
#include "sql_builder.hpp"
#include "types.hpp"
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dearsql {

struct CreateDatabaseOptions {
    std::string name;
    std::string comment;
    // PostgreSQL
    std::string owner;
    std::string templateDb;
    std::string encoding;
    std::string tablespace;
    // MySQL/MariaDB
    std::string charset;
    std::string collation;
};

class IDatabase;
using DatabasePtr = std::shared_ptr<IDatabase>;

using Status = std::pair<bool, std::string>;

// thrown by catalog and data calls; execute() reports errors in QueryResult instead
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/**
 * @brief Synchronous server-level connection.
 *
 * For server-backed databases (Postgres, MySQL, Mongo, Redis, MSSQL, Oracle,
 * Cassandra, Redshift) this represents the server-wide handle and exposes the
 * list of databases/keyspaces. For SQLite it wraps a single file and
 * `databases()` returns exactly one entry.
 */
class IConnection {
public:
    virtual ~IConnection() = default;

    virtual Status open() = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool isOpen() const = 0;

    [[nodiscard]] virtual DatabaseType type() const = 0;
    [[nodiscard]] virtual const ConnectionInfo& info() const = 0;

    // List of databases/keyspaces. Lazy: each handle is cheap and opens its
    // underlying session/pool on first metadata call.
    virtual std::vector<DatabasePtr> databases() = 0;

    // Get a database by name. Empty = default.
    virtual DatabasePtr database(const std::string& name = "") = 0;

    // Fresh, uncached handle with its own connection, for hosts that pool
    // per-worker connections. Backends without one fall back to database().
    virtual DatabasePtr openDatabase(const std::string& name = "") {
        return database(name);
    }

    virtual Status createDatabase(const CreateDatabaseOptions& opts) {
        return {false, "createDatabase not supported for this database type"};
    }
    virtual Status dropDatabase(const std::string& name) {
        return {false, "dropDatabase not supported for this database type"};
    }
    virtual Status renameDatabase(const std::string& oldName, const std::string& newName) {
        return {false, "renameDatabase not supported for this database type"};
    }
};

/**
 * @brief A table-holder: a database, schema, keyspace, or Mongo db.
 *
 * One interface covers two conceptual levels — a database AND (for Postgres /
 * MSSQL) a schema inside it. PostgreSQL's `IDatabase::schemas()` returns
 * sub-`IDatabase` handles, one per schema; every other backend's `schemas()`
 * returns empty (schemas don't exist there — use `tables()` / `views()` on
 * the database itself).
 *
 *   conn.databases()                          → vector<DatabasePtr>
 *   db.schemas()  // postgres only            → vector<DatabasePtr>
 *   db.tables() / db.views() / ...            → catalog listings
 */
class IDatabase {
public:
    virtual ~IDatabase() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual DatabaseType type() const = 0;

    // PostgreSQL & MSSQL: returns the schemas inside this database, each
    // exposed as another IDatabase. All other backends: returns empty.
    virtual std::vector<DatabasePtr> schemas() {
        return {};
    }

    // one schema of this database bound to the same connection (Postgres/MSSQL).
    // backends override with a cheap constructor; the default lists schemas()
    virtual DatabasePtr schema(const std::string& schemaName) {
        for (auto& s : schemas()) {
            if (s->name() == schemaName)
                return s;
        }
        return nullptr;
    }

    // Catalog listings (across all schemas, or directly for non-schema backends).
    virtual std::vector<Table> tables() = 0;
    virtual std::vector<Table> views() = 0;
    virtual std::vector<Table> materializedViews() {
        return {};
    }
    virtual std::vector<std::string> sequences() {
        return {};
    }
    virtual std::vector<Routine> routines() {
        return {};
    }

    // Refresh a single table with full column/index/FK details.
    virtual Table describeTable(const std::string& tableName) = 0;
    // the table's CREATE TABLE statement and its indexes, runnable as-is. the server's
    // own DDL where it has one (MySQL, SQLite, DuckDB, Oracle, Redshift, Postgres
    // catalog), else built from describeTable. throws Error (also where tables have
    // no DDL: MongoDB, Redis)
    virtual std::string tableDdl(const std::string& tableName);

    virtual QueryResult execute(const std::string& sql, int rowLimit = 1000) = 0;

    // false when the session behind this handle is gone; pools reopen it
    virtual bool alive() {
        return true;
    }

    // best-effort server-side cancel of whatever this handle is running, called
    // from another thread (KILL QUERY, PQcancel, ...)
    virtual void cancel() {}

    // schema used to qualify unqualified names in the builder defaults below;
    // empty for backends without schemas
    [[nodiscard]] virtual std::string schemaName() const {
        return "";
    }

    // data access and DDL default to the dialect's SQL builder over execute();
    // they throw dearsql::Error on failure. Mongo/Redis override them.
    virtual std::vector<std::vector<std::string>>
    getTableData(const Table& table, int limit, int offset, const std::string& whereClause = "",
                 const std::string& orderByClause = "");
    virtual std::vector<std::string> getColumnNames(const Table& table);
    virtual int getRowCount(const Table& table, const std::string& whereClause = "");

    virtual Status createTable(const Table& table);
    virtual Status renameTable(const std::string& oldName, const std::string& newName);
    virtual Status dropTable(const std::string& tableName);
    virtual Status truncateTable(const std::string& tableName);
    virtual Status dropColumn(const std::string& tableName, const std::string& columnName);
    virtual Status addColumn(const Table& table, const Column& column) {
        if (type() == DatabaseType::MONGODB || type() == DatabaseType::REDIS)
            return {false, "addColumn not supported for this database"};
        auto builder = createSQLBuilder(type());
        const auto sql = builder->addColumn(builder->qualifiedName(table), column);
        if (sql.empty())
            return {false, "addColumn not supported for this database"};
        auto r = execute(sql, 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    virtual Status renameColumn(const Table& table, const std::string& oldColumnName,
                                const std::string& newColumnName) {
        if (type() == DatabaseType::MONGODB || type() == DatabaseType::REDIS)
            return {false, "renameColumn not supported for this database"};
        auto builder = createSQLBuilder(type());
        const auto sql =
            builder->renameColumn(builder->qualifiedName(table), oldColumnName, newColumnName);
        if (sql.empty())
            return {false, "renameColumn not supported for this database"};
        auto r = execute(sql, 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    virtual Status alterColumn(const Table& table, const std::string& oldColumnName,
                               const Column& newColumn) {
        if (type() == DatabaseType::MONGODB || type() == DatabaseType::REDIS)
            return {false, "alterColumn not supported for this database"};
        auto builder = createSQLBuilder(type());
        const auto sql =
            builder->alterColumn(builder->qualifiedName(table), oldColumnName, newColumn);
        if (sql.empty())
            return {false, "alterColumn not supported for this database"};
        auto r = execute(sql, 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    virtual Status insertRow(const Table& table, const std::vector<std::string>& columnNames,
                             const std::vector<std::string>& valueLiterals) {
        if (type() == DatabaseType::MONGODB || type() == DatabaseType::REDIS)
            return {false, "insertRow not supported for this database"};
        auto builder = createSQLBuilder(type());
        auto r = execute(builder->insertRow(builder->qualifiedName(table), columnNames,
                                            valueLiterals),
                         0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    virtual Status updateRow(
        const Table& table, const std::vector<std::pair<std::string, std::string>>& assignments,
        const std::string& whereExpr) {
        if (type() == DatabaseType::MONGODB || type() == DatabaseType::REDIS)
            return {false, "updateRow not supported for this database"};
        if (whereExpr.empty())
            return {false, "updateRow requires a WHERE expression"};
        auto builder = createSQLBuilder(type());
        auto r = execute(builder->updateRow(builder->qualifiedName(table), assignments, whereExpr),
                         0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    virtual Status deleteRow(const Table& table, const std::string& whereExpr) {
        if (type() == DatabaseType::MONGODB || type() == DatabaseType::REDIS)
            return {false, "deleteRow not supported for this database"};
        if (whereExpr.empty())
            return {false, "deleteRow requires a WHERE expression"};
        auto builder = createSQLBuilder(type());
        auto r = execute(builder->deleteRow(builder->qualifiedName(table), whereExpr), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    virtual Status dropView(const std::string& viewName, bool isMaterialized = false);
    // Postgres/MSSQL only.
    virtual Status renameSchema(const std::string& newName) {
        return {false, "renameSchema not supported for this database"};
    }
    virtual Status dropSchema() {
        return {false, "dropSchema not supported for this database"};
    }
};

} // namespace dearsql
