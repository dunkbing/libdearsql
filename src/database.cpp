#include "dearsql/database.hpp"

namespace dearsql {

namespace {

Status toStatus(const QueryResult& r) {
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

const StatementResult& firstOrThrow(const QueryResult& r) {
    if (!r.success())
        throw Error(r.errorMessage().empty() ? "query failed" : r.errorMessage());
    return r.statements.front();
}

} // namespace

std::vector<std::vector<std::string>> IDatabase::getTableData(const Table& table, int limit,
                                                              int offset,
                                                              const std::string& whereClause,
                                                              const std::string& orderByClause) {
    auto builder = createSQLBuilder(type());
    auto r = execute(builder->selectAll(table, whereClause, orderByClause, limit, offset), 0);
    return firstOrThrow(r).tableData;
}

std::vector<std::string> IDatabase::getColumnNames(const Table& table) {
    auto r = execute(createSQLBuilder(type())->columnNames(table), 0);
    std::vector<std::string> names;
    for (const auto& row : firstOrThrow(r).tableData) {
        if (!row.empty())
            names.push_back(row[0]);
    }
    return names;
}

int IDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    auto r = execute(createSQLBuilder(type())->countRows(table, whereClause), 1);
    const auto& rows = firstOrThrow(r).tableData;
    if (rows.empty() || rows[0].empty())
        return 0;
    try {
        return std::stoi(rows[0][0]);
    } catch (...) {
        return 0;
    }
}

Status IDatabase::createTable(const Table& table) {
    auto builder = createSQLBuilder(type());
    const std::string schema = table.schema.empty() ? schemaName() : table.schema;
    return toStatus(execute(builder->createTable(table, schema), 0));
}

Status IDatabase::renameTable(const std::string& oldName, const std::string& newName) {
    return toStatus(execute(createSQLBuilder(type())->renameTable(schemaName(), oldName, newName), 0));
}

Status IDatabase::dropTable(const std::string& tableName) {
    return toStatus(execute(createSQLBuilder(type())->dropTable(schemaName(), tableName), 0));
}

Status IDatabase::truncateTable(const std::string& tableName) {
    return toStatus(execute(createSQLBuilder(type())->truncateTable(schemaName(), tableName), 0));
}

Status IDatabase::dropColumn(const std::string& tableName, const std::string& columnName) {
    auto builder = createSQLBuilder(type());
    Table t;
    t.name = tableName;
    t.schema = schemaName();
    return toStatus(execute(builder->dropColumn(builder->qualifiedName(t), columnName), 0));
}

Status IDatabase::dropView(const std::string& viewName, bool isMaterialized) {
    auto builder = createSQLBuilder(type());
    Table t;
    t.name = viewName;
    t.schema = schemaName();
    return toStatus(execute(std::string(isMaterialized ? "DROP MATERIALIZED VIEW " : "DROP VIEW ") +
                                builder->qualifiedName(t),
                            0));
}

} // namespace dearsql
