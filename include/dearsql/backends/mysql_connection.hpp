#pragma once

#include "dearsql/database.hpp"
#include <mutex>

struct st_mysql; // MYSQL from <mysql/mysql.h>, kept out of the public header

namespace dearsql {

// One database handle over one MYSQL* connection. `database()` hands out a
// cached one per name; `openDatabase()` a fresh one, so a host can pool them
// and run queries in parallel. Exposes the raw handle for callers that need
// session-scoped state (dump import/export).
class MySQLDatabase final : public IDatabase {
public:
    MySQLDatabase(ConnectionInfo info, std::string dbName);
    ~MySQLDatabase() override;

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return info_.type;
    }

    // connect now instead of on first use
    Status open();
    [[nodiscard]] bool isOpen() const {
        return conn_ != nullptr;
    }
    // round trip; false when the server dropped the session
    bool ping();
    // raw libmysql handle, null until open
    [[nodiscard]] st_mysql* handle() const {
        return conn_;
    }
    // KILL QUERY on this handle's server thread, from a throwaway connection
    void cancel() override;

    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<Routine> routines() override;
    Table describeTable(const std::string& tableName) override;

    // phaseTimings: network latency, execution, data download, data parse
    QueryResult execute(const std::string& sql, int rowLimit) override;

    std::vector<std::vector<std::string>>
    getTableData(const Table& table, int limit, int offset, const std::string& whereClause,
                 const std::string& orderByClause) override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause) override;

    Status createTable(const Table& table) override;
    Status renameTable(const std::string& oldName, const std::string& newName) override;
    Status dropTable(const std::string& tableName) override;
    Status truncateTable(const std::string& tableName) override;
    Status dropColumn(const std::string& tableName, const std::string& columnName) override;
    Status dropView(const std::string& viewName, bool isMaterialized) override;

private:
    void ensureConn(); // throws

    ConnectionInfo info_;
    std::string name_;
    st_mysql* conn_ = nullptr;
    std::mutex mu_;
};

// MySQL / MariaDB backend.
class MySQLConnection final : public IConnection {
public:
    explicit MySQLConnection(const ConnectionInfo& info);
    ~MySQLConnection() override;

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

    Status createDatabase(const CreateDatabaseOptions& opts) override;
    // dropping the connected database leaves the connection with no default
    // schema (name() == ""), as a user may lack grants on `mysql`
    Status dropDatabase(const std::string& name) override;
    Status renameDatabase(const std::string& oldName, const std::string& newName) override;

private:
    ConnectionInfo info_;
    void* impl_ = nullptr;
};

} // namespace dearsql
