#include "dearsql/backends/sqlite_connection.hpp"

#include <chrono>
#include <format>
#include <memory>
#include <sqlite3.h>
#include <stdexcept>

namespace dearsql {

namespace {

struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const {
        if (stmt)
            sqlite3_finalize(stmt);
    }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

std::string columnText(sqlite3_stmt* stmt, int col, bool sentinelNull = false) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL)
        return sentinelNull ? std::string(NULL_SENTINEL) : "NULL";
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? text : "";
}

template <typename RowCallback>
void queryRows(sqlite3* db, const std::string& sql, RowCallback&& cb) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    StmtPtr stmt(raw);
    while (sqlite3_step(raw) == SQLITE_ROW)
        cb(raw);
}

int queryInt(sqlite3* db, const std::string& sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db));
    StmtPtr stmt(raw);
    if (sqlite3_step(raw) == SQLITE_ROW)
        return sqlite3_column_int(raw, 0);
    return 0;
}

int64_t getTableSizeBytes(sqlite3* db, const std::string& tableName) {
    static constexpr const char* sql = "SELECT SUM(d.pgsize) FROM sqlite_master m "
                                       "LEFT JOIN dbstat d ON d.name = m.name "
                                       "WHERE m.tbl_name = ? AND m.type IN ('table', 'index')";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
        return -1;
    StmtPtr stmt(raw);
    sqlite3_bind_text(raw, 1, tableName.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_ROW)
        return -1;
    if (sqlite3_column_type(raw, 0) == SQLITE_NULL)
        return -1;
    return sqlite3_column_int64(raw, 0);
}

std::string quoteIdent(const std::string& ident) {
    std::string out = "\"";
    for (char c : ident) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    out += "\"";
    return out;
}

QueryResult runMultiStatement(sqlite3* db, const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!db) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "Database not connected";
        result.statements.push_back(std::move(r));
        return result;
    }

    const char* remaining = query.c_str();
    while (remaining && *remaining) {
        while (*remaining && (*remaining == ' ' || *remaining == '\n' || *remaining == '\r' ||
                              *remaining == '\t'))
            ++remaining;
        if (!*remaining)
            break;

        sqlite3_stmt* raw = nullptr;
        const char* tail = nullptr;
        int rc = sqlite3_prepare_v2(db, remaining, -1, &raw, &tail);
        if (rc != SQLITE_OK) {
            StatementResult r;
            r.success = false;
            r.errorMessage = sqlite3_errmsg(db);
            result.statements.push_back(std::move(r));
            break;
        }
        if (!raw) {
            remaining = tail;
            continue;
        }

        StmtPtr stmt(raw);
        StatementResult r;
        const int colCount = sqlite3_column_count(raw);
        if (colCount > 0) {
            for (int i = 0; i < colCount; ++i)
                r.columnNames.emplace_back(sqlite3_column_name(raw, i));
            int rowCount = 0;
            while (sqlite3_step(raw) == SQLITE_ROW && rowCount < rowLimit) {
                std::vector<std::string> row;
                row.reserve(colCount);
                for (int i = 0; i < colCount; ++i)
                    row.push_back(columnText(raw, i, true));
                r.tableData.push_back(std::move(row));
                ++rowCount;
            }
            r.message = std::format("Returned {} row{}", r.tableData.size(),
                                    r.tableData.size() == 1 ? "" : "s");
        } else {
            rc = sqlite3_step(raw);
            if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
                r.affectedRows = sqlite3_changes(db);
                r.message = "Query executed successfully";
            } else {
                r.success = false;
                r.errorMessage = sqlite3_errmsg(db);
            }
        }
        result.statements.push_back(std::move(r));
        remaining = tail;
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return result;
}

// SQLite has no schema layer — tables/views/etc. live directly under the
// database. One class implements IDatabase end-to-end.
class SQLiteDatabaseNode final : public IDatabase {
public:
    SQLiteDatabaseNode(sqlite3* db, std::string connName, std::string fileName)
        : db_(db), name_(std::move(fileName)), connName_(std::move(connName)) {}

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::SQLITE;
    }

    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<std::string> sequences() override;
    Table describeTable(const std::string& tableName) override;

    QueryResult execute(const std::string& sql, int rowLimit) override {
        return runMultiStatement(db_, sql, rowLimit);
    }

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
                                                       const std::string& orderByClause) override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause) override;

    Status createTable(const Table& table) override;
    Status renameTable(const std::string& oldName, const std::string& newName) override;
    Status dropTable(const std::string& tableName) override;
    Status dropColumn(const std::string& tableName, const std::string& columnName) override;
    Status dropView(const std::string& viewName, bool isMaterialized) override;

private:
    sqlite3* db_;
    std::string name_;
    std::string connName_;
};

std::vector<Table> SQLiteDatabaseNode::tables() {
    std::vector<Table> result;
    if (!db_)
        return result;

    try {
        std::vector<std::string> tableNames;
        queryRows(db_,
                  "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
                  [&](sqlite3_stmt* stmt) { tableNames.push_back(columnText(stmt, 0)); });

        for (const auto& tn : tableNames) {
            Table t;
            t.name = tn;
            t.fullName = connName_ + "." + tn;

            queryRows(db_, std::format("PRAGMA table_info({})", tn), [&](sqlite3_stmt* stmt) {
                Column c;
                c.name = columnText(stmt, 1);
                c.type = columnText(stmt, 2);
                c.isNotNull = columnText(stmt, 3) == "1";
                c.defaultValue = columnText(stmt, 4);
                c.isPrimaryKey = columnText(stmt, 5) == "1";
                t.columns.push_back(std::move(c));
            });

            queryRows(db_, std::format("PRAGMA foreign_key_list({})", tn), [&](sqlite3_stmt* stmt) {
                ForeignKey fk;
                fk.targetTable = columnText(stmt, 2);
                fk.sourceColumn = columnText(stmt, 3);
                fk.targetColumn = columnText(stmt, 4);
                fk.onUpdate = columnText(stmt, 5);
                fk.onDelete = columnText(stmt, 6);
                fk.name = std::format("fk_{}_{}", tn, fk.sourceColumn);
                t.foreignKeys.push_back(std::move(fk));
            });

            queryRows(db_, std::format("PRAGMA index_list({})", tn), [&](sqlite3_stmt* stmt) {
                Index idx;
                idx.name = columnText(stmt, 1);
                idx.isUnique = columnText(stmt, 2) == "1";
                if (idx.name.find("sqlite_autoindex") != std::string::npos)
                    idx.isPrimary = true;
                queryRows(db_, std::format("PRAGMA index_info({})", idx.name),
                          [&](sqlite3_stmt* infoStmt) {
                              idx.columns.push_back(columnText(infoStmt, 2));
                          });
                idx.type = "BTREE";
                t.indexes.push_back(std::move(idx));
            });

            t.sizeBytes = getTableSizeBytes(db_, tn);
            buildForeignKeyLookup(t);
            result.push_back(std::move(t));
        }

        populateIncomingForeignKeys(result);
    } catch (...) {
    }
    return result;
}

std::vector<Table> SQLiteDatabaseNode::views() {
    std::vector<Table> result;
    if (!db_)
        return result;
    try {
        std::vector<std::string> viewNames;
        queryRows(db_, "SELECT name FROM sqlite_master WHERE type='view'",
                  [&](sqlite3_stmt* stmt) { viewNames.push_back(columnText(stmt, 0)); });
        for (const auto& vn : viewNames) {
            Table v;
            v.name = vn;
            v.fullName = connName_ + "." + vn;
            queryRows(db_, std::format("PRAGMA table_info({})", vn), [&](sqlite3_stmt* stmt) {
                Column c;
                c.name = columnText(stmt, 1);
                c.type = columnText(stmt, 2);
                c.isNotNull = columnText(stmt, 3) == "1";
                v.columns.push_back(std::move(c));
            });
            result.push_back(std::move(v));
        }
    } catch (...) {
    }
    return result;
}

std::vector<std::string> SQLiteDatabaseNode::sequences() {
    std::vector<std::string> result;
    if (!db_)
        return result;
    try {
        bool exists = false;
        queryRows(db_,
                  "SELECT name FROM sqlite_master WHERE type='table' AND name='sqlite_sequence'",
                  [&](sqlite3_stmt*) { exists = true; });
        if (!exists)
            return result;
        queryRows(db_, "SELECT name FROM sqlite_sequence ORDER BY name",
                  [&](sqlite3_stmt* stmt) { result.push_back(columnText(stmt, 0)); });
    } catch (...) {
    }
    return result;
}

Table SQLiteDatabaseNode::describeTable(const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.fullName = connName_ + "." + tableName;
    if (!db_)
        return t;
    try {
        queryRows(db_, std::format("PRAGMA table_info(\"{}\")", tableName),
                  [&](sqlite3_stmt* stmt) {
                      Column c;
                      c.name = columnText(stmt, 1);
                      c.type = columnText(stmt, 2);
                      c.isNotNull = sqlite3_column_int(stmt, 3) != 0;
                      c.defaultValue = columnText(stmt, 4);
                      c.isPrimaryKey = sqlite3_column_int(stmt, 5) != 0;
                      t.columns.push_back(std::move(c));
                  });
        queryRows(db_, std::format("PRAGMA foreign_key_list(\"{}\")", tableName),
                  [&](sqlite3_stmt* stmt) {
                      ForeignKey fk;
                      fk.targetTable = columnText(stmt, 2);
                      fk.sourceColumn = columnText(stmt, 3);
                      fk.targetColumn = columnText(stmt, 4);
                      fk.onUpdate = columnText(stmt, 5);
                      fk.onDelete = columnText(stmt, 6);
                      fk.name = std::format("fk_{}_{}", tableName, fk.sourceColumn);
                      t.foreignKeys.push_back(std::move(fk));
                  });
        queryRows(db_, std::format("PRAGMA index_list(\"{}\")", tableName),
                  [&](sqlite3_stmt* stmt) {
                      Index idx;
                      idx.name = columnText(stmt, 1);
                      idx.isUnique = columnText(stmt, 2) == "1";
                      queryRows(db_, std::format("PRAGMA index_info(\"{}\")", idx.name),
                                [&](sqlite3_stmt* infoStmt) {
                                    idx.columns.push_back(columnText(infoStmt, 2));
                                });
                      idx.type = "BTREE";
                      t.indexes.push_back(std::move(idx));
                  });
        t.sizeBytes = getTableSizeBytes(db_, tableName);
        buildForeignKeyLookup(t);
    } catch (...) {
    }
    return t;
}

std::vector<std::vector<std::string>>
SQLiteDatabaseNode::getTableData(const Table& table, int limit, int offset,
                                 const std::string& whereClause,
                                 const std::string& orderByClause) {
    std::vector<std::vector<std::string>> data;
    if (!db_)
        return data;

    std::string sql = "SELECT * FROM " + quoteIdent(table.name);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    if (!orderByClause.empty())
        sql += " ORDER BY " + orderByClause;
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);

    try {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
            return data;
        StmtPtr stmt(raw);
        const int colCount = sqlite3_column_count(raw);
        while (sqlite3_step(raw) == SQLITE_ROW) {
            std::vector<std::string> row;
            row.reserve(colCount);
            for (int i = 0; i < colCount; ++i)
                row.push_back(columnText(raw, i, true));
            data.push_back(std::move(row));
        }
    } catch (...) {
    }
    return data;
}

std::vector<std::string> SQLiteDatabaseNode::getColumnNames(const Table& table) {
    std::vector<std::string> names;
    if (!db_)
        return names;
    try {
        queryRows(db_, std::format("PRAGMA table_info(\"{}\")", table.name),
                  [&](sqlite3_stmt* stmt) { names.emplace_back(columnText(stmt, 1)); });
    } catch (...) {
    }
    return names;
}

int SQLiteDatabaseNode::getRowCount(const Table& table, const std::string& whereClause) {
    if (!db_)
        return 0;
    std::string sql = "SELECT COUNT(*) FROM " + quoteIdent(table.name);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    try {
        return queryInt(db_, sql);
    } catch (...) {
        return 0;
    }
}

Status SQLiteDatabaseNode::createTable(const Table& table) {
    if (!db_)
        return {false, "Database not connected"};

    std::string sql = "CREATE TABLE " + quoteIdent(table.name) + " (";
    bool first = true;
    for (const auto& c : table.columns) {
        if (!first)
            sql += ", ";
        first = false;
        sql += quoteIdent(c.name) + " " + (c.type.empty() ? std::string("TEXT") : c.type);
        if (c.isPrimaryKey)
            sql += " PRIMARY KEY";
        if (c.isAutoIncrement)
            sql += " AUTOINCREMENT";
        if (c.isNotNull && !c.isPrimaryKey)
            sql += " NOT NULL";
        if (c.isUnique && !c.isPrimaryKey)
            sql += " UNIQUE";
        if (!c.defaultValue.empty())
            sql += " DEFAULT " + c.defaultValue;
    }
    for (const auto& fk : table.foreignKeys) {
        sql += ", FOREIGN KEY (" + quoteIdent(fk.sourceColumn) + ") REFERENCES " +
               quoteIdent(fk.targetTable) + "(" + quoteIdent(fk.targetColumn) + ")";
        if (!fk.onUpdate.empty())
            sql += " ON UPDATE " + fk.onUpdate;
        if (!fk.onDelete.empty())
            sql += " ON DELETE " + fk.onDelete;
    }
    sql += ")";

    auto r = runMultiStatement(db_, sql, 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status SQLiteDatabaseNode::renameTable(const std::string& oldName, const std::string& newName) {
    if (!db_)
        return {false, "Database not connected"};
    auto r = runMultiStatement(
        db_, "ALTER TABLE " + quoteIdent(oldName) + " RENAME TO " + quoteIdent(newName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status SQLiteDatabaseNode::dropTable(const std::string& tableName) {
    if (!db_)
        return {false, "Database not connected"};
    auto r = runMultiStatement(db_, "DROP TABLE " + quoteIdent(tableName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status SQLiteDatabaseNode::dropColumn(const std::string& tableName, const std::string& columnName) {
    if (!db_)
        return {false, "Database not connected"};
    auto r = runMultiStatement(
        db_, "ALTER TABLE " + quoteIdent(tableName) + " DROP COLUMN " + quoteIdent(columnName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status SQLiteDatabaseNode::dropView(const std::string& viewName, bool /*isMaterialized*/) {
    if (!db_)
        return {false, "Database not connected"};
    auto r = runMultiStatement(db_, "DROP VIEW " + quoteIdent(viewName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

} // namespace

// ---------- SQLiteConnection ----------

SQLiteConnection::SQLiteConnection(const ConnectionInfo& info) : info_(info) {}

SQLiteConnection::~SQLiteConnection() {
    SQLiteConnection::close();
}

Status SQLiteConnection::open() {
    if (db_)
        return {true, ""};
    int rc = sqlite3_open_v2(info_.path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db_ ? sqlite3_errmsg(db_) : "Unable to open database";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return {false, err};
    }
    defaultDb_.reset();
    return {true, ""};
}

void SQLiteConnection::close() {
    defaultDb_.reset();
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SQLiteConnection::isOpen() const {
    return db_ != nullptr;
}

std::vector<DatabasePtr> SQLiteConnection::databases() {
    if (auto db = database())
        return {db};
    return {};
}

DatabasePtr SQLiteConnection::database(const std::string& /*name*/) {
    if (!db_)
        return nullptr;
    if (!defaultDb_) {
        const auto& p = info_.path;
        auto pos = p.find_last_of("/\\");
        std::string fileName = (pos == std::string::npos) ? p : p.substr(pos + 1);
        defaultDb_ = std::make_shared<SQLiteDatabaseNode>(db_, info_.name, std::move(fileName));
    }
    return defaultDb_;
}

} // namespace dearsql
