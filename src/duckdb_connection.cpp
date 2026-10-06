#include "dearsql/backends/duckdb_connection.hpp"

#include <algorithm>
#include <chrono>
#include <duckdb.h>
#include <filesystem>
#include <format>
#include <mutex>

namespace dearsql {

namespace {

struct Db {
    duckdb_database db = nullptr;
    ~Db() {
        if (db)
            duckdb_close(&db);
    }
};

struct ResultGuard {
    duckdb_result res{};
    ~ResultGuard() {
        duckdb_destroy_result(&res);
    }
};

std::string cellText(duckdb_result* res, idx_t col, idx_t row) {
    if (duckdb_value_is_null(res, col, row))
        return std::string(NULL_SENTINEL);
    char* v = duckdb_value_varchar(res, col, row);
    std::string out = v ? v : "";
    duckdb_free(v);
    return out;
}

std::string literal(const std::string& s) {
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}

std::string quoteIdent(const std::string& s) {
    std::string out = "\"";
    for (char c : s)
        out += c == '"' ? std::string("\"\"") : std::string(1, c);
    return out + "\"";
}

bool truthy(const std::string& v) {
    return v == "true" || v == "1";
}

// "[a, b]" -> {a, b}; LIST columns are cast to VARCHAR in the query because
// duckdb_value_varchar renders lists as empty strings
std::vector<std::string> parseList(const std::string& v) {
    std::vector<std::string> out;
    std::string s = v;
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']')
        s = s.substr(1, s.size() - 2);
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find(',', start);
        if (end == std::string::npos)
            end = s.size();
        std::string item = s.substr(start, end - start);
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (!item.empty())
            out.push_back(item);
        start = end + 1;
    }
    return out;
}

class DuckDBDatabaseNode final : public IDatabase {
public:
    DuckDBDatabaseNode(std::shared_ptr<Db> db, duckdb_connection con, std::string name)
        : db_(std::move(db)), con_(con), name_(std::move(name)) {}
    ~DuckDBDatabaseNode() override {
        duckdb_disconnect(&con_);
    }

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::DUCKDB;
    }

    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<std::string> sequences() override;
    Table describeTable(const std::string& tableName) override;
    QueryResult execute(const std::string& sql, int rowLimit) override;
    void cancel() override {
        duckdb_interrupt(con_);
    }

    // rows of a catalog query; throws on error
    std::vector<std::vector<std::string>> rows(const std::string& sql);

private:
    Table load(const std::string& name, bool isView);

    std::shared_ptr<Db> db_;
    duckdb_connection con_;
    std::string name_;
    std::mutex mutex_; // a duckdb connection is not safe for concurrent use
};

std::vector<std::vector<std::string>> DuckDBDatabaseNode::rows(const std::string& sql) {
    std::lock_guard lock(mutex_);
    ResultGuard g;
    if (duckdb_query(con_, sql.c_str(), &g.res) == DuckDBError) {
        const char* err = duckdb_result_error(&g.res);
        throw Error(err ? err : "query failed");
    }
    const idx_t cols = duckdb_column_count(&g.res);
    const idx_t n = duckdb_row_count(&g.res);
    std::vector<std::vector<std::string>> out(n);
    for (idx_t r = 0; r < n; ++r) {
        out[r].reserve(cols);
        for (idx_t c = 0; c < cols; ++c)
            out[r].push_back(cellText(&g.res, c, r));
    }
    return out;
}

QueryResult DuckDBDatabaseNode::execute(const std::string& query, int rowLimit) {
    QueryResult result;
    const auto start = std::chrono::steady_clock::now();
    std::lock_guard lock(mutex_);

    duckdb_extracted_statements stmts = nullptr;
    const idx_t count = duckdb_extract_statements(con_, query.c_str(), &stmts);
    if (count == 0) {
        StatementResult r;
        r.success = false;
        const char* err = stmts ? duckdb_extract_statements_error(stmts) : nullptr;
        r.errorMessage = err ? err : "Failed to parse query";
        result.statements.push_back(std::move(r));
        duckdb_destroy_extracted(&stmts);
        return result;
    }

    for (idx_t i = 0; i < count; ++i) {
        StatementResult r;
        duckdb_prepared_statement prep = nullptr;
        if (duckdb_prepare_extracted_statement(con_, stmts, i, &prep) == DuckDBError) {
            const char* err = duckdb_prepare_error(prep);
            r.success = false;
            r.errorMessage = err ? err : "Failed to prepare statement";
            duckdb_destroy_prepare(&prep);
            result.statements.push_back(std::move(r));
            break;
        }
        ResultGuard g;
        const bool failed = duckdb_execute_prepared(prep, &g.res) == DuckDBError;
        duckdb_destroy_prepare(&prep);
        if (failed) {
            const char* err = duckdb_result_error(&g.res);
            r.success = false;
            r.errorMessage = err ? err : "Query failed";
            result.statements.push_back(std::move(r));
            break;
        }

        // DML results also carry a "Count" column, so classify by return type
        if (duckdb_result_return_type(g.res) == DUCKDB_RESULT_TYPE_QUERY_RESULT) {
            const idx_t cols = duckdb_column_count(&g.res);
            idx_t n = duckdb_row_count(&g.res);
            if (rowLimit > 0)
                n = std::min<idx_t>(n, static_cast<idx_t>(rowLimit));
            for (idx_t c = 0; c < cols; ++c) {
                const char* name = duckdb_column_name(&g.res, c);
                r.columnNames.emplace_back(name ? name : "");
            }
            for (idx_t row = 0; row < n; ++row) {
                std::vector<std::string> data;
                data.reserve(cols);
                for (idx_t c = 0; c < cols; ++c)
                    data.push_back(cellText(&g.res, c, row));
                r.tableData.push_back(std::move(data));
            }
            r.message = std::format("Returned {} row{}", r.tableData.size(),
                                    r.tableData.size() == 1 ? "" : "s");
        } else {
            r.affectedRows = static_cast<int>(duckdb_rows_changed(&g.res));
            r.message = "Query executed successfully";
        }
        result.statements.push_back(std::move(r));
    }
    duckdb_destroy_extracted(&stmts);

    result.executionTimeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    return result;
}

Table DuckDBDatabaseNode::load(const std::string& tableName, bool isView) {
    Table t;
    t.name = tableName;
    t.fullName = tableName;
    const std::string lit = literal(tableName);

    for (const auto& row : rows("PRAGMA table_info(" + lit + ")")) {
        if (row.size() < 6)
            continue;
        Column c;
        c.name = row[1];
        c.type = row[2];
        c.isNotNull = truthy(row[3]);
        if (row[4] != NULL_SENTINEL)
            c.defaultValue = row[4];
        c.isPrimaryKey = truthy(row[5]);
        t.columns.push_back(std::move(c));
    }
    if (isView)
        return t;

    for (const auto& row :
         rows("SELECT index_name, is_unique, is_primary, expressions::VARCHAR FROM duckdb_indexes() "
              "WHERE schema_name = 'main' AND table_name = " +
              lit)) {
        Index idx;
        idx.name = row[0];
        idx.isUnique = truthy(row[1]);
        idx.isPrimary = truthy(row[2]);
        for (auto& col : parseList(row[3])) {
            if (col.size() >= 2 && col.front() == '"' && col.back() == '"')
                col = col.substr(1, col.size() - 2);
            idx.columns.push_back(col);
        }
        idx.type = "ART";
        t.indexes.push_back(std::move(idx));
    }

    for (const auto& row : rows("SELECT constraint_column_names::VARCHAR, referenced_table, "
                                "referenced_column_names::VARCHAR FROM duckdb_constraints() "
                                "WHERE constraint_type = 'FOREIGN KEY' AND schema_name = 'main' "
                                "AND table_name = " +
                                lit)) {
        const auto src = parseList(row[0]);
        const auto dst = parseList(row[2]);
        for (size_t i = 0; i < src.size() && i < dst.size(); ++i) {
            ForeignKey fk;
            fk.sourceColumn = src[i];
            fk.targetTable = row[1];
            fk.targetColumn = dst[i];
            fk.name = std::format("fk_{}_{}", tableName, src[i]);
            t.foreignKeys.push_back(std::move(fk));
        }
    }
    buildForeignKeyLookup(t);
    return t;
}

std::vector<Table> DuckDBDatabaseNode::tables() {
    std::vector<Table> result;
    for (const auto& row : rows("SELECT table_name FROM information_schema.tables "
                                "WHERE table_type = 'BASE TABLE' AND table_schema = 'main' "
                                "ORDER BY table_name"))
        result.push_back(load(row[0], false));
    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> DuckDBDatabaseNode::views() {
    std::vector<Table> result;
    for (const auto& row : rows("SELECT view_name, sql FROM duckdb_views() "
                                "WHERE schema_name = 'main' AND NOT internal ORDER BY view_name")) {
        Table v = load(row[0], true);
        if (auto pos = row[1].find(" AS "); pos != std::string::npos)
            v.definition = row[1].substr(pos + 4);
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<std::string> DuckDBDatabaseNode::sequences() {
    std::vector<std::string> result;
    for (const auto& row :
         rows("SELECT sequence_name FROM duckdb_sequences() ORDER BY sequence_name"))
        result.push_back(row[0]);
    return result;
}

Table DuckDBDatabaseNode::describeTable(const std::string& tableName) {
    return load(tableName, false);
}

std::string stemOf(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}

} // namespace

DuckDBConnection::DuckDBConnection(const ConnectionInfo& info) : info_(info) {}

DuckDBConnection::~DuckDBConnection() {
    DuckDBConnection::close();
}

Status DuckDBConnection::open() {
    if (db_)
        return {true, ""};

    // ponytail: csv is a materialized in-memory copy; reopening re-imports the file
    const bool csv = isCsvPath(info_.path);
    auto db = std::make_shared<Db>();

    duckdb_config config = nullptr;
    if (info_.readOnly && !csv) {
        duckdb_create_config(&config);
        duckdb_set_config(config, "access_mode", "READ_ONLY");
    }
    char* errMsg = nullptr;
    const auto state =
        duckdb_open_ext(csv ? ":memory:" : info_.path.c_str(), &db->db, config, &errMsg);
    if (config)
        duckdb_destroy_config(&config);
    if (state == DuckDBError) {
        std::string error = errMsg ? errMsg : "Unable to open database";
        duckdb_free(errMsg);
        db->db = nullptr;
        return {false, error};
    }
    duckdb_free(errMsg);
    db_ = db;

    if (csv) {
        auto r = database()->execute(std::format("CREATE TABLE {} AS SELECT * FROM read_csv({})",
                                                 quoteIdent(stemOf(info_.path)),
                                                 literal(info_.path)),
                                     0);
        if (!r.success()) {
            close();
            return {false, "Failed to import CSV: " + r.errorMessage()};
        }
    }
    return {true, ""};
}

void DuckDBConnection::close() {
    defaultDb_.reset();
    db_.reset(); // handed-out handles keep the database alive
}

bool DuckDBConnection::isOpen() const {
    return db_ != nullptr;
}

std::vector<DatabasePtr> DuckDBConnection::databases() {
    if (auto db = database())
        return {db};
    return {};
}

DatabasePtr DuckDBConnection::database(const std::string& name) {
    if (!defaultDb_)
        defaultDb_ = openDatabase(name);
    return defaultDb_;
}

DatabasePtr DuckDBConnection::openDatabase(const std::string& /*name*/) {
    if (!db_)
        return nullptr;
    auto db = std::static_pointer_cast<Db>(db_);
    duckdb_connection con = nullptr;
    if (duckdb_connect(db->db, &con) == DuckDBError)
        return nullptr;
    return std::make_shared<DuckDBDatabaseNode>(db, con, std::filesystem::path(info_.path).filename().string());
}

} // namespace dearsql
