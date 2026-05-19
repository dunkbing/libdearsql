#pragma once

#include "connection_info.hpp"
#include "types.hpp"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dearsql {

[[nodiscard]] std::string autoIncrementClause(DatabaseType type);
[[nodiscard]] bool supportsAutoIncrement(DatabaseType dbType, const std::string& columnType);

class ISQLBuilder {
public:
    virtual ~ISQLBuilder() = default;

    [[nodiscard]] virtual DatabaseType databaseType() const = 0;
    [[nodiscard]] virtual std::string quoteIdentifier(const std::string& identifier) const;
    [[nodiscard]] virtual std::string qualifiedName(const Table& table) const;

    [[nodiscard]] std::string createTable(const Table& table,
                                          const std::string& schemaPrefix = "") const;
    [[nodiscard]] virtual std::string addColumn(const std::string& qualifiedTable,
                                                const Column& column) const = 0;
    [[nodiscard]] virtual std::string dropColumn(const std::string& qualifiedTable,
                                                 const std::string& columnName) const = 0;

    [[nodiscard]] virtual std::string selectAll(const Table& table, const std::string& whereClause,
                                                const std::string& orderByClause, int limit,
                                                int offset) const;
    [[nodiscard]] virtual std::string countRows(const Table& table,
                                                const std::string& whereClause) const;
    [[nodiscard]] virtual std::string columnNames(const Table& table) const = 0;

    [[nodiscard]] virtual std::string
    insertRow(const std::string& qualifiedTable, const std::vector<std::string>& columnNames,
              const std::vector<std::string>& valueLiterals) const;
    [[nodiscard]] virtual std::string
    updateRow(const std::string& qualifiedTable,
              const std::vector<std::pair<std::string, std::string>>& assignments,
              const std::string& whereExpr) const;
    [[nodiscard]] virtual std::string deleteRow(const std::string& qualifiedTable,
                                                const std::string& whereExpr) const;

    [[nodiscard]] virtual std::string renameTable(const std::string& schema,
                                                  const std::string& oldName,
                                                  const std::string& newName) const;
    [[nodiscard]] virtual std::string dropTable(const std::string& schema,
                                                const std::string& tableName) const;
    [[nodiscard]] virtual std::string truncateTable(const std::string& schema,
                                                    const std::string& tableName) const;

    [[nodiscard]] virtual std::string renameColumn(const std::string& qualifiedTable,
                                                   const std::string& oldColumnName,
                                                   const std::string& newColumnName) const;
    [[nodiscard]] virtual std::string alterColumn(const std::string& qualifiedTable,
                                                  const std::string& oldColumnName,
                                                  const Column& newColumn) const;

protected:
    [[nodiscard]] std::string qualifiedRef(const std::string& schema,
                                           const std::string& tableName) const;
};

std::unique_ptr<ISQLBuilder> createSQLBuilder(DatabaseType type);

class PostgreSQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::POSTGRESQL;
    }
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;
    [[nodiscard]] std::string truncateTable(const std::string& schema,
                                            const std::string& tableName) const override;
};

class MySQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::MYSQL;
    }
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;
    [[nodiscard]] std::string renameTable(const std::string& schema, const std::string& oldName,
                                          const std::string& newName) const override;
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class MSSQLBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::MSSQL;
    }
    [[nodiscard]] std::string quoteIdentifier(const std::string& identifier) const override;
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;
    [[nodiscard]] std::string selectAll(const Table& table, const std::string& whereClause,
                                        const std::string& orderByClause, int limit,
                                        int offset) const override;
    [[nodiscard]] std::string renameTable(const std::string& schema, const std::string& oldName,
                                          const std::string& newName) const override;
    [[nodiscard]] std::string renameColumn(const std::string& qualifiedTable,
                                           const std::string& oldColumnName,
                                           const std::string& newColumnName) const override;
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class OracleBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::ORACLE;
    }
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;
    [[nodiscard]] std::string selectAll(const Table& table, const std::string& whereClause,
                                        const std::string& orderByClause, int limit,
                                        int offset) const override;
    [[nodiscard]] std::string dropTable(const std::string& schema,
                                        const std::string& tableName) const override;
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class SQLiteBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::SQLITE;
    }
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;
    [[nodiscard]] std::string alterColumn(const std::string& qualifiedTable,
                                          const std::string& oldColumnName,
                                          const Column& newColumn) const override;
};

class CassandraBuilder : public ISQLBuilder {
public:
    [[nodiscard]] DatabaseType databaseType() const override {
        return DatabaseType::CASSANDRA;
    }
    [[nodiscard]] std::string addColumn(const std::string& table,
                                        const Column& column) const override;
    [[nodiscard]] std::string dropColumn(const std::string& table,
                                         const std::string& columnName) const override;
    [[nodiscard]] std::string columnNames(const Table& table) const override;
    [[nodiscard]] std::string selectAll(const Table& table, const std::string& whereClause,
                                        const std::string& orderByClause, int limit,
                                        int offset) const override;
    [[nodiscard]] std::string dropTable(const std::string& schema,
                                        const std::string& tableName) const override;
};

} // namespace dearsql
