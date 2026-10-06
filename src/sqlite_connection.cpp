#include "dearsql/backends/sqlite_connection.hpp"

#include <chrono>
#include <format>
#include <memory>
#include <sqlite3.h>

namespace dearsql {

namespace {

struct StmtDeleter {
    void operator()(sqlite3_stmt* stmt) const {
        if (stmt)
            sqlite3_finalize(stmt);
    }
};
using StmtPtr = std::unique_ptr<sqlite3_stmt, StmtDeleter>;

// owns one sqlite3 handle; shared by the connection and the node it hands out
struct Handle {
    sqlite3* db = nullptr;
    ~Handle() {
        if (db)
            sqlite3_close(db);
    }
};

Status openHandle(const ConnectionInfo& info, sqlite3** out) {
    const int flags = (info.readOnly ? SQLITE_OPEN_READONLY
                                     : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                      SQLITE_OPEN_FULLMUTEX;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(info.path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "Unable to open database";
        sqlite3_close(db);
        return {false, err};
    }
    *out = db;
    return {true, ""};
}

std::string columnText(sqlite3_stmt* stmt, int col, bool sentinelNull = false) {
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL)
        return sentinelNull ? std::string(NULL_SENTINEL) : "";
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    return text ? text : "";
}

template <typename RowCallback>
void queryRows(sqlite3* db, const std::string& sql, RowCallback&& cb) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
        throw Error(sqlite3_errmsg(db));
    StmtPtr stmt(raw);
    int rc;
    while ((rc = sqlite3_step(raw)) == SQLITE_ROW)
        cb(raw);
    if (rc != SQLITE_DONE)
        throw Error(sqlite3_errmsg(db));
}

int64_t getTableSizeBytes(sqlite3* db, const std::string& tableName) {
    static constexpr const char* sql = "SELECT SUM(d.pgsize) FROM sqlite_master m "
                                       "LEFT JOIN dbstat d ON d.name = m.name "
                                       "WHERE m.tbl_name = ? AND m.type IN ('table', 'index')";
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK)
        return -1; // dbstat not compiled in
    StmtPtr stmt(raw);
    sqlite3_bind_text(raw, 1, tableName.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(raw) != SQLITE_ROW || sqlite3_column_type(raw, 0) == SQLITE_NULL)
        return -1;
    return sqlite3_column_int64(raw, 0);
}

std::string quoteIdent(const std::string& ident) {
    std::string out = "\"";
    for (char c : ident)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

QueryResult runMultiStatement(sqlite3* db, const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::steady_clock::now();

    const char* remaining = query.c_str();
    while (remaining && *remaining) {
        while (*remaining && std::isspace(static_cast<unsigned char>(*remaining)))
            ++remaining;
        if (!*remaining)
            break;

        sqlite3_stmt* raw = nullptr;
        const char* tail = nullptr;
        if (sqlite3_prepare_v2(db, remaining, -1, &raw, &tail) != SQLITE_OK) {
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
            int rc;
            while ((rowLimit <= 0 || static_cast<int>(r.tableData.size()) < rowLimit) &&
                   (rc = sqlite3_step(raw)) == SQLITE_ROW) {
                std::vector<std::string> row;
                row.reserve(colCount);
                for (int i = 0; i < colCount; ++i)
                    row.push_back(columnText(raw, i, true));
                r.tableData.push_back(std::move(row));
            }
            if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
                r.success = false;
                r.errorMessage = sqlite3_errmsg(db);
            }
            r.message = std::format("Returned {} row{}", r.tableData.size(),
                                    r.tableData.size() == 1 ? "" : "s");
        } else {
            const int rc = sqlite3_step(raw);
            if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
                r.affectedRows = sqlite3_changes(db);
                r.message = "Query executed successfully";
            } else {
                r.success = false;
                r.errorMessage = sqlite3_errmsg(db);
            }
        }
        const bool failed = !r.success;
        result.statements.push_back(std::move(r));
        if (failed)
            break;
        remaining = tail;
    }

    result.executionTimeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime)
            .count();
    return result;
}

// SQLite has no schema layer: tables and views live directly under the file
class SQLiteDatabaseNode final : public IDatabase {
public:
    SQLiteDatabaseNode(std::shared_ptr<Handle> handle, std::string connName, std::string fileName)
        : h_(std::move(handle)), name_(std::move(fileName)), connName_(std::move(connName)) {}

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
        return runMultiStatement(h_->db, sql, rowLimit);
    }
    void cancel() override {
        sqlite3_interrupt(h_->db);
    }

private:
    Table load(const std::string& name, bool isView);

    std::shared_ptr<Handle> h_;
    std::string name_;
    std::string connName_;
};

Table SQLiteDatabaseNode::load(const std::string& tn, bool isView) {
    sqlite3* db = h_->db;
    const std::string q = quoteIdent(tn);
    Table t;
    t.name = tn;
    t.fullName = connName_ + "." + tn;

    queryRows(db, "PRAGMA table_info(" + q + ")", [&](sqlite3_stmt* stmt) {
        Column c;
        c.name = columnText(stmt, 1);
        c.type = columnText(stmt, 2);
        c.isNotNull = sqlite3_column_int(stmt, 3) != 0;
        c.defaultValue = columnText(stmt, 4);
        c.isPrimaryKey = sqlite3_column_int(stmt, 5) != 0;
        t.columns.push_back(std::move(c));
    });
    if (isView)
        return t;

    queryRows(db, "PRAGMA foreign_key_list(" + q + ")", [&](sqlite3_stmt* stmt) {
        ForeignKey fk;
        fk.targetTable = columnText(stmt, 2);
        fk.sourceColumn = columnText(stmt, 3);
        fk.targetColumn = columnText(stmt, 4);
        fk.onUpdate = columnText(stmt, 5);
        fk.onDelete = columnText(stmt, 6);
        fk.name = std::format("fk_{}_{}", tn, fk.sourceColumn);
        t.foreignKeys.push_back(std::move(fk));
    });

    std::vector<Index> indexes;
    queryRows(db, "PRAGMA index_list(" + q + ")", [&](sqlite3_stmt* stmt) {
        Index idx;
        idx.name = columnText(stmt, 1);
        idx.isUnique = sqlite3_column_int(stmt, 2) != 0;
        idx.isPrimary = columnText(stmt, 3) == "pk";
        idx.type = "BTREE";
        indexes.push_back(std::move(idx));
    });
    for (auto& idx : indexes) {
        queryRows(db, "PRAGMA index_info(" + quoteIdent(idx.name) + ")",
                  [&](sqlite3_stmt* stmt) { idx.columns.push_back(columnText(stmt, 2)); });
    }
    t.indexes = std::move(indexes);
    t.sizeBytes = getTableSizeBytes(db, tn);
    buildForeignKeyLookup(t);
    return t;
}

std::vector<Table> SQLiteDatabaseNode::tables() {
    std::vector<std::string> names;
    queryRows(h_->db,
              "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' "
              "ORDER BY name",
              [&](sqlite3_stmt* stmt) { names.push_back(columnText(stmt, 0)); });
    std::vector<Table> result;
    result.reserve(names.size());
    for (const auto& n : names)
        result.push_back(load(n, false));
    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> SQLiteDatabaseNode::views() {
    std::vector<std::pair<std::string, std::string>> rows;
    queryRows(h_->db, "SELECT name, sql FROM sqlite_master WHERE type='view' ORDER BY name",
              [&](sqlite3_stmt* stmt) { rows.emplace_back(columnText(stmt, 0), columnText(stmt, 1)); });
    std::vector<Table> result;
    for (const auto& [n, sql] : rows) {
        Table v = load(n, true);
        // body after the first AS, like the other backends
        if (auto pos = sql.find(" AS "); pos != std::string::npos)
            v.definition = sql.substr(pos + 4);
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<std::string> SQLiteDatabaseNode::sequences() {
    std::vector<std::string> result;
    bool exists = false;
    queryRows(h_->db,
              "SELECT name FROM sqlite_master WHERE type='table' AND name='sqlite_sequence'",
              [&](sqlite3_stmt*) { exists = true; });
    if (exists) {
        queryRows(h_->db, "SELECT name FROM sqlite_sequence ORDER BY name",
                  [&](sqlite3_stmt* stmt) { result.push_back(columnText(stmt, 0)); });
    }
    return result;
}

Table SQLiteDatabaseNode::describeTable(const std::string& tableName) {
    return load(tableName, false);
}

std::string fileNameOf(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    return pos == std::string::npos ? p : p.substr(pos + 1);
}

} // namespace

SQLiteConnection::SQLiteConnection(const ConnectionInfo& info) : info_(info) {}

SQLiteConnection::~SQLiteConnection() {
    SQLiteConnection::close();
}

Status SQLiteConnection::open() {
    if (handle_)
        return {true, ""};
    sqlite3* db = nullptr;
    auto status = openHandle(info_, &db);
    if (!status.first)
        return status;
    auto h = std::make_shared<Handle>();
    h->db = db;
    handle_ = std::move(h);
    defaultDb_.reset();
    return {true, ""};
}

void SQLiteConnection::close() {
    defaultDb_.reset();
    handle_.reset(); // nodes handed out keep their own reference
}

bool SQLiteConnection::isOpen() const {
    return handle_ != nullptr;
}

sqlite3* SQLiteConnection::handle() const {
    return handle_ ? static_cast<Handle*>(handle_.get())->db : nullptr;
}

std::vector<DatabasePtr> SQLiteConnection::databases() {
    if (auto db = database())
        return {db};
    return {};
}

DatabasePtr SQLiteConnection::database(const std::string& /*name*/) {
    if (!handle_)
        return nullptr;
    if (!defaultDb_) {
        defaultDb_ = std::make_shared<SQLiteDatabaseNode>(std::static_pointer_cast<Handle>(handle_),
                                                          info_.name, fileNameOf(info_.path));
    }
    return defaultDb_;
}

DatabasePtr SQLiteConnection::openDatabase(const std::string& /*name*/) {
    sqlite3* db = nullptr;
    if (!openHandle(info_, &db).first)
        return nullptr;
    auto h = std::make_shared<Handle>();
    h->db = db;
    return std::make_shared<SQLiteDatabaseNode>(std::move(h), info_.name, fileNameOf(info_.path));
}

} // namespace dearsql
