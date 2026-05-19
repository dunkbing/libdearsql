#pragma once

#include "connection_info.hpp"
#include "query_result.hpp"
#include "types.hpp"
#include <memory>
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

    virtual QueryResult execute(const std::string& sql, int rowLimit = 1000) = 0;

    virtual std::vector<std::vector<std::string>>
    getTableData(const Table& table, int limit, int offset, const std::string& whereClause = "",
                 const std::string& orderByClause = "") = 0;

    virtual std::vector<std::string> getColumnNames(const Table& table) = 0;
    virtual int getRowCount(const Table& table, const std::string& whereClause = "") = 0;

    virtual Status createTable(const Table& table) {
        return {false, "createTable not supported for this database"};
    }
    virtual Status renameTable(const std::string& oldName, const std::string& newName) {
        return {false, "renameTable not supported for this database"};
    }
    virtual Status dropTable(const std::string& tableName) {
        return {false, "dropTable not supported for this database"};
    }
    virtual Status truncateTable(const std::string& tableName) {
        return {false, "truncateTable not supported for this database"};
    }
    virtual Status dropColumn(const std::string& tableName, const std::string& columnName) {
        return {false, "dropColumn not supported for this database"};
    }
    virtual Status dropView(const std::string& viewName, bool isMaterialized = false) {
        return {false, "dropView not supported for this database"};
    }
    // Postgres/MSSQL only.
    virtual Status renameSchema(const std::string& newName) {
        return {false, "renameSchema not supported for this database"};
    }
    virtual Status dropSchema() {
        return {false, "dropSchema not supported for this database"};
    }
};

} // namespace dearsql
