#include "dearsql/backends/postgres_connection.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <libpq-fe.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace dearsql {

namespace {

struct PgResultDeleter {
    void operator()(PGresult* r) const {
        if (r)
            PQclear(r);
    }
};
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

std::string escapeLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\'')
            out += "''";
        else
            out += c;
    }
    return out;
}

std::string quoteIdent(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"')
            out += "\"\"";
        else
            out += c;
    }
    out += "\"";
    return out;
}

StatementResult extractPgResult(PGresult* res, int rowLimit) {
    StatementResult result;
    ExecStatusType status = PQresultStatus(res);

    if (status == PGRES_TUPLES_OK) {
        const int nFields = PQnfields(res);
        const int nRows = PQntuples(res);
        std::vector<bool> isBoolCol(nFields, false);
        for (int col = 0; col < nFields; col++) {
            result.columnNames.emplace_back(PQfname(res, col));
            isBoolCol[col] = (PQftype(res, col) == 16); // BOOLOID
        }
        const int limit = std::min(nRows, rowLimit);
        for (int row = 0; row < limit; row++) {
            std::vector<std::string> rowData;
            rowData.reserve(nFields);
            for (int col = 0; col < nFields; col++) {
                if (PQgetisnull(res, row, col)) {
                    rowData.emplace_back(NULL_SENTINEL);
                } else if (isBoolCol[col]) {
                    const char* v = PQgetvalue(res, row, col);
                    rowData.emplace_back(v[0] == 't' ? BOOL_TRUE_SENTINEL : BOOL_FALSE_SENTINEL);
                } else {
                    rowData.emplace_back(PQgetvalue(res, row, col));
                }
            }
            result.tableData.push_back(std::move(rowData));
        }
        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
    } else if (status == PGRES_COMMAND_OK) {
        const char* affected = PQcmdTuples(res);
        if (affected && *affected)
            result.message = std::format("{} row(s) affected", affected);
        else
            result.message = "Query executed successfully";
    } else {
        result.success = false;
        result.errorMessage = PQresultErrorMessage(res);
    }
    return result;
}

class PostgresDatabase;

// Helpers that operate on a PostgresDatabase. Defined out-of-line below.
QueryResult dbExecute(PostgresDatabase& db, const std::string& sql, int rowLimit);
std::vector<Table> loadTablesForSchema(PostgresDatabase& db, const std::string& schema);
std::vector<Table> loadViewsForSchema(PostgresDatabase& db, const std::string& schema);
std::vector<Table> loadMatViewsForSchema(PostgresDatabase& db, const std::string& schema);
std::vector<std::string> loadSequencesForSchema(PostgresDatabase& db, const std::string& schema);
std::vector<Routine> loadRoutinesForSchema(PostgresDatabase& db, const std::string& schema);
Table describeTableInSchema(PostgresDatabase& db, const std::string& schema,
                            const std::string& tableName);

// Per-database handle. Owns its own PGconn (libpq sessions scoped to one
// database). Inherits from enable_shared_from_this so its schemas() method
// can hand a self-reference into the PostgresSchema objects it builds.
class PostgresDatabase final : public IDatabase,
                               public std::enable_shared_from_this<PostgresDatabase> {
public:
    PostgresDatabase(ConnectionInfo info, std::string dbName)
        : info_(std::move(info)), name_(std::move(dbName)) {}
    ~PostgresDatabase() override {
        if (conn_)
            PQfinish(conn_);
    }

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return info_.type;
    }

    std::vector<DatabasePtr> schemas() override;
    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<Table> materializedViews() override;
    std::vector<std::string> sequences() override;
    std::vector<Routine> routines() override;

    Table describeTable(const std::string& tableName) override;
    QueryResult execute(const std::string& sql, int rowLimit) override;

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
                                                       const std::string& orderByClause) override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause) override;

    Status createTable(const Table& table) override;
    Status renameTable(const std::string& oldName, const std::string& newName) override;
    Status dropTable(const std::string& tableName) override;
    Status truncateTable(const std::string& tableName) override;
    Status dropColumn(const std::string& tableName, const std::string& columnName) override;
    Status dropView(const std::string& viewName, bool isMaterialized) override;

    PGconn* handle() {
        ensureConn();
        return conn_;
    }
    std::mutex& mutex() {
        return mu_;
    }

private:
    void ensureConn() {
        if (conn_)
            return;
        auto connStr = info_.buildConnectionString(name_);
        conn_ = PQconnectdb(connStr.c_str());
        if (PQstatus(conn_) != CONNECTION_OK) {
            std::string err = PQerrorMessage(conn_);
            PQfinish(conn_);
            conn_ = nullptr;
            throw std::runtime_error("PostgreSQL connection failed: " + err);
        }
    }

    std::string fullyQualified(const Table& t) const {
        std::string out;
        if (!t.schema.empty())
            out = quoteIdent(t.schema) + ".";
        return out + quoteIdent(t.name);
    }

    ConnectionInfo info_;
    std::string name_;
    PGconn* conn_ = nullptr;
    std::mutex mu_;
};

// One schema inside a Postgres database. Holds a shared_ptr to its parent so
// the parent connection outlives the schema handle.
class PostgresSchema final : public IDatabase {
public:
    PostgresSchema(std::shared_ptr<PostgresDatabase> parent, std::string schemaName)
        : parent_(std::move(parent)), name_(std::move(schemaName)) {}

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return parent_->type();
    }

    std::vector<Table> tables() override {
        auto t = loadTablesForSchema(*parent_, name_);
        for (auto& x : t)
            x.fullName = parent_->name() + "." + x.schema + "." + x.name;
        return t;
    }
    std::vector<Table> views() override {
        return loadViewsForSchema(*parent_, name_);
    }
    std::vector<Table> materializedViews() override {
        return loadMatViewsForSchema(*parent_, name_);
    }
    std::vector<std::string> sequences() override {
        return loadSequencesForSchema(*parent_, name_);
    }
    std::vector<Routine> routines() override {
        return loadRoutinesForSchema(*parent_, name_);
    }
    Table describeTable(const std::string& tableName) override {
        return describeTableInSchema(*parent_, name_, tableName);
    }

    QueryResult execute(const std::string& sql, int rowLimit) override {
        std::string wrapped = std::format("SET search_path TO {}; ", quoteIdent(name_)) + sql;
        auto r = parent_->execute(wrapped, rowLimit);
        if (r.statements.size() > 1)
            r.statements.erase(r.statements.begin());
        return r;
    }

    std::vector<std::vector<std::string>>
    getTableData(const Table& table, int limit, int offset, const std::string& whereClause,
                 const std::string& orderByClause) override {
        Table t = table;
        t.schema = name_;
        return parent_->getTableData(t, limit, offset, whereClause, orderByClause);
    }
    std::vector<std::string> getColumnNames(const Table& table) override {
        Table t = table;
        t.schema = name_;
        return parent_->getColumnNames(t);
    }
    int getRowCount(const Table& table, const std::string& whereClause) override {
        Table t = table;
        t.schema = name_;
        return parent_->getRowCount(t, whereClause);
    }

    Status createTable(const Table& table) override {
        Table t = table;
        t.schema = name_;
        return parent_->createTable(t);
    }
    Status renameTable(const std::string& oldName, const std::string& newName) override {
        auto r = parent_->execute(std::format(R"(ALTER TABLE {}.{} RENAME TO {})", quoteIdent(name_),
                                              quoteIdent(oldName), quoteIdent(newName)),
                                  0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status dropTable(const std::string& tableName) override {
        auto r = parent_->execute(
            std::format(R"(DROP TABLE {}.{})", quoteIdent(name_), quoteIdent(tableName)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status truncateTable(const std::string& tableName) override {
        auto r = parent_->execute(std::format(R"(TRUNCATE TABLE ONLY {}.{})", quoteIdent(name_),
                                              quoteIdent(tableName)),
                                  0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status dropColumn(const std::string& tableName, const std::string& columnName) override {
        auto r = parent_->execute(std::format(R"(ALTER TABLE {}.{} DROP COLUMN {})",
                                              quoteIdent(name_), quoteIdent(tableName),
                                              quoteIdent(columnName)),
                                  0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status dropView(const std::string& viewName, bool isMaterialized) override {
        const char* kw = isMaterialized ? "MATERIALIZED VIEW" : "VIEW";
        auto r = parent_->execute(
            std::format(R"(DROP {} {}.{})", kw, quoteIdent(name_), quoteIdent(viewName)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status renameSchema(const std::string& newName) override {
        auto r = parent_->execute(std::format(R"(ALTER SCHEMA {} RENAME TO {})", quoteIdent(name_),
                                              quoteIdent(newName)),
                                  0);
        if (r.success())
            name_ = newName;
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status dropSchema() override {
        auto r =
            parent_->execute(std::format(R"(DROP SCHEMA {} CASCADE)", quoteIdent(name_)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    std::shared_ptr<PostgresDatabase> parent_;
    std::string name_;
};

// ---------- helpers using PostgresDatabase ----------

QueryResult dbExecute(PostgresDatabase& db, const std::string& sql, int rowLimit) {
    return db.execute(sql, rowLimit);
}

std::vector<Table> loadTablesForSchema(PostgresDatabase& db, const std::string& schema) {
    std::vector<Table> result;
    auto names = db.execute(
        std::format("SELECT tablename FROM pg_tables WHERE schemaname = '{}' ORDER BY tablename",
                    escapeLiteral(schema)),
        10000);
    if (!names.success())
        return result;

    std::vector<std::string> tableNames;
    if (!names.empty())
        for (const auto& row : names[0].tableData)
            if (!row.empty())
                tableNames.push_back(row[0]);
    if (tableNames.empty())
        return result;

    std::string nameList;
    for (size_t i = 0; i < tableNames.size(); ++i) {
        if (i)
            nameList += ", ";
        nameList += "'" + escapeLiteral(tableNames[i]) + "'";
    }

    std::unordered_map<std::string, std::vector<Column>> cols;
    {
        auto r = db.execute(
            std::format("SELECT c.table_name, c.column_name, c.data_type, c.is_nullable, "
                        "CASE WHEN tc.constraint_type = 'PRIMARY KEY' THEN 'true' ELSE 'false' END, "
                        "CASE WHEN c.column_default LIKE 'nextval(%' OR c.is_identity = 'YES' "
                        "  THEN 'true' ELSE 'false' END, "
                        "COALESCE(c.column_default, '') "
                        "FROM information_schema.columns c "
                        "LEFT JOIN information_schema.key_column_usage kcu "
                        "  ON c.column_name = kcu.column_name AND c.table_name = kcu.table_name "
                        "  AND c.table_schema = kcu.table_schema "
                        "LEFT JOIN information_schema.table_constraints tc "
                        "  ON kcu.constraint_name = tc.constraint_name "
                        "  AND tc.constraint_type = 'PRIMARY KEY' "
                        "WHERE c.table_schema = '{}' AND c.table_name IN ({}) "
                        "ORDER BY c.table_name, c.ordinal_position",
                        escapeLiteral(schema), nameList),
            100000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 7)
                    continue;
                Column c;
                c.name = row[1];
                c.type = row[2];
                c.isNotNull = row[3] == "NO";
                c.isPrimaryKey = row[4] == "true";
                c.isAutoIncrement = row[5] == "true";
                c.defaultValue = row[6];
                cols[row[0]].push_back(std::move(c));
            }
        }
    }

    std::unordered_map<std::string, std::vector<ForeignKey>> fks;
    {
        auto r = db.execute(
            std::format("SELECT tc.table_name, kcu.column_name, ccu.table_name, ccu.column_name, "
                        "tc.constraint_name "
                        "FROM information_schema.table_constraints tc "
                        "JOIN information_schema.key_column_usage kcu "
                        "  ON tc.constraint_name = kcu.constraint_name "
                        "  AND tc.table_schema = kcu.table_schema "
                        "JOIN information_schema.constraint_column_usage ccu "
                        "  ON ccu.constraint_name = tc.constraint_name "
                        "  AND ccu.table_schema = tc.table_schema "
                        "WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_schema = '{}' "
                        "AND tc.table_name IN ({})",
                        escapeLiteral(schema), nameList),
            100000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 5)
                    continue;
                ForeignKey fk;
                fk.sourceColumn = row[1];
                fk.targetTable = row[2];
                fk.targetColumn = row[3];
                fk.name = row[4];
                fks[row[0]].push_back(std::move(fk));
            }
        }
    }

    for (const auto& tn : tableNames) {
        Table t;
        t.name = tn;
        t.schema = schema;
        t.fullName = schema + "." + tn;
        if (auto it = cols.find(tn); it != cols.end())
            t.columns = std::move(it->second);
        if (auto it = fks.find(tn); it != fks.end())
            t.foreignKeys = std::move(it->second);
        buildForeignKeyLookup(t);
        result.push_back(std::move(t));
    }
    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> loadViewsForSchema(PostgresDatabase& db, const std::string& schema) {
    std::vector<Table> result;
    auto r = db.execute(
        std::format("SELECT viewname, COALESCE(definition, '') FROM pg_views "
                    "WHERE schemaname = '{}' ORDER BY viewname",
                    escapeLiteral(schema)),
        10000);
    if (!r.success() || r.empty())
        return result;
    for (const auto& row : r[0].tableData) {
        if (row.size() < 2)
            continue;
        Table v;
        v.name = row[0];
        v.schema = schema;
        v.fullName = schema + "." + row[0];
        v.definition = row[1];
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<Table> loadMatViewsForSchema(PostgresDatabase& db, const std::string& schema) {
    std::vector<Table> result;
    auto r = db.execute(
        std::format("SELECT matviewname, COALESCE(definition, '') FROM pg_matviews "
                    "WHERE schemaname = '{}' ORDER BY matviewname",
                    escapeLiteral(schema)),
        10000);
    if (!r.success() || r.empty())
        return result;
    for (const auto& row : r[0].tableData) {
        if (row.size() < 2)
            continue;
        Table v;
        v.name = row[0];
        v.schema = schema;
        v.fullName = schema + "." + row[0];
        v.definition = row[1];
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<std::string> loadSequencesForSchema(PostgresDatabase& db, const std::string& schema) {
    std::vector<std::string> out;
    auto r = db.execute(std::format("SELECT sequencename FROM pg_sequences "
                                    "WHERE schemaname = '{}' ORDER BY sequencename",
                                    escapeLiteral(schema)),
                        10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData)
        if (!row.empty())
            out.push_back(row[0]);
    return out;
}

std::vector<Routine> loadRoutinesForSchema(PostgresDatabase& db, const std::string& schema) {
    std::vector<Routine> out;
    auto r = db.execute(
        std::format(
            "SELECT p.proname, "
            "p.proname || '(' || COALESCE(pg_catalog.pg_get_function_identity_arguments(p.oid), '') || ')', "
            "CASE p.prokind WHEN 'f' THEN 'FUNCTION' ELSE 'PROCEDURE' END, "
            "pg_catalog.pg_get_function_result(p.oid) "
            "FROM pg_catalog.pg_proc p "
            "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
            "WHERE n.nspname = '{}' AND p.prokind IN ('f', 'p') ORDER BY p.proname",
            escapeLiteral(schema)),
        10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData) {
        if (row.size() < 4)
            continue;
        Routine rt;
        rt.name = row[0];
        rt.signature = row[1];
        rt.kind = row[2] == "PROCEDURE" ? RoutineKind::Procedure : RoutineKind::Function;
        rt.returnType = row[3];
        out.push_back(std::move(rt));
    }
    return out;
}

Table describeTableInSchema(PostgresDatabase& db, const std::string& schema,
                            const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.schema = schema;

    auto cols = db.execute(
        std::format("SELECT column_name, data_type, is_nullable, COALESCE(column_default, ''), "
                    "CASE WHEN column_default LIKE 'nextval(%' OR is_identity = 'YES' "
                    "  THEN 'true' ELSE 'false' END "
                    "FROM information_schema.columns "
                    "WHERE table_schema = '{}' AND table_name = '{}' ORDER BY ordinal_position",
                    escapeLiteral(schema), escapeLiteral(tableName)),
        10000);
    if (cols.success() && !cols.empty()) {
        for (const auto& row : cols[0].tableData) {
            if (row.size() < 5)
                continue;
            Column c;
            c.name = row[0];
            c.type = row[1];
            c.isNotNull = row[2] == "NO";
            c.defaultValue = row[3];
            c.isAutoIncrement = row[4] == "true";
            t.columns.push_back(std::move(c));
        }
    }

    auto pk = db.execute(
        std::format("SELECT a.attname FROM pg_index i "
                    "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey) "
                    "WHERE i.indrelid = '{}.{}'::regclass AND i.indisprimary",
                    quoteIdent(schema), quoteIdent(tableName)),
        10000);
    if (pk.success() && !pk.empty()) {
        std::vector<std::string> pkCols;
        for (const auto& row : pk[0].tableData)
            if (!row.empty())
                pkCols.push_back(row[0]);
        for (auto& c : t.columns)
            if (std::find(pkCols.begin(), pkCols.end(), c.name) != pkCols.end())
                c.isPrimaryKey = true;
    }

    auto fks = db.execute(
        std::format("SELECT kcu.column_name, ccu.table_name, ccu.column_name, tc.constraint_name "
                    "FROM information_schema.table_constraints tc "
                    "JOIN information_schema.key_column_usage kcu "
                    "  ON tc.constraint_name = kcu.constraint_name "
                    "  AND tc.table_schema = kcu.table_schema "
                    "JOIN information_schema.constraint_column_usage ccu "
                    "  ON ccu.constraint_name = tc.constraint_name "
                    "  AND ccu.table_schema = tc.table_schema "
                    "WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_schema = '{}' "
                    "AND tc.table_name = '{}'",
                    escapeLiteral(schema), escapeLiteral(tableName)),
        10000);
    if (fks.success() && !fks.empty()) {
        for (const auto& row : fks[0].tableData) {
            if (row.size() < 4)
                continue;
            ForeignKey fk;
            fk.sourceColumn = row[0];
            fk.targetTable = row[1];
            fk.targetColumn = row[2];
            fk.name = row[3];
            t.foreignKeys.push_back(std::move(fk));
        }
    }
    buildForeignKeyLookup(t);
    return t;
}

} // namespace

// ---------- PostgresDatabase member impls ----------

QueryResult PostgresDatabase::execute(const std::string& sql, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();
    try {
        ensureConn();
    } catch (const std::exception& e) {
        StatementResult s;
        s.success = false;
        s.errorMessage = e.what();
        result.statements.push_back(std::move(s));
        return result;
    }
    std::lock_guard lock(mu_);
    if (!PQsendQuery(conn_, sql.c_str())) {
        StatementResult s;
        s.success = false;
        s.errorMessage = PQerrorMessage(conn_);
        result.statements.push_back(std::move(s));
        return result;
    }
    while (PGresult* raw = PQgetResult(conn_)) {
        PgResultPtr res(raw);
        auto r = extractPgResult(res.get(), rowLimit);
        if (r.success || !r.errorMessage.empty())
            result.statements.push_back(std::move(r));
    }
    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return result;
}

std::vector<DatabasePtr> PostgresDatabase::schemas() {
    std::vector<DatabasePtr> out;
    auto r = execute("SELECT nspname FROM pg_namespace "
                     "WHERE nspname NOT LIKE 'pg_%' AND nspname <> 'information_schema' "
                     "ORDER BY nspname",
                     10000);
    if (!r.success() || r.empty())
        return out;
    auto self = shared_from_this();
    for (const auto& row : r[0].tableData) {
        if (row.empty())
            continue;
        out.push_back(std::make_shared<PostgresSchema>(self, row[0]));
    }
    return out;
}

std::vector<Table> PostgresDatabase::tables() {
    std::vector<Table> out;
    auto schemasList = execute("SELECT nspname FROM pg_namespace "
                               "WHERE nspname NOT LIKE 'pg_%' AND nspname <> 'information_schema' "
                               "ORDER BY nspname",
                               10000);
    if (!schemasList.success() || schemasList.empty())
        return out;
    for (const auto& row : schemasList[0].tableData) {
        if (row.empty())
            continue;
        auto ts = loadTablesForSchema(*this, row[0]);
        out.insert(out.end(), ts.begin(), ts.end());
    }
    return out;
}

std::vector<Table> PostgresDatabase::views() {
    std::vector<Table> out;
    auto r = execute("SELECT table_schema, table_name FROM information_schema.views "
                     "WHERE table_schema NOT IN ('pg_catalog', 'information_schema') "
                     "ORDER BY table_schema, table_name",
                     10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData) {
        if (row.size() < 2)
            continue;
        Table v;
        v.schema = row[0];
        v.name = row[1];
        v.fullName = name_ + "." + v.schema + "." + v.name;
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<Table> PostgresDatabase::materializedViews() {
    std::vector<Table> out;
    auto r = execute("SELECT schemaname, matviewname FROM pg_matviews "
                     "ORDER BY schemaname, matviewname",
                     10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData) {
        if (row.size() < 2)
            continue;
        Table v;
        v.schema = row[0];
        v.name = row[1];
        v.fullName = name_ + "." + v.schema + "." + v.name;
        out.push_back(std::move(v));
    }
    return out;
}

std::vector<std::string> PostgresDatabase::sequences() {
    std::vector<std::string> out;
    auto r = execute("SELECT schemaname || '.' || sequencename FROM pg_sequences "
                     "ORDER BY schemaname, sequencename",
                     10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData)
        if (!row.empty())
            out.push_back(row[0]);
    return out;
}

std::vector<Routine> PostgresDatabase::routines() {
    std::vector<Routine> out;
    auto r = execute(
        "SELECT p.proname, "
        "p.proname || '(' || COALESCE(pg_catalog.pg_get_function_identity_arguments(p.oid), '') || ')', "
        "CASE p.prokind WHEN 'f' THEN 'FUNCTION' ELSE 'PROCEDURE' END, "
        "pg_catalog.pg_get_function_result(p.oid) "
        "FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "WHERE n.nspname NOT IN ('pg_catalog', 'information_schema') "
        "AND p.prokind IN ('f', 'p') ORDER BY n.nspname, p.proname",
        10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData) {
        if (row.size() < 4)
            continue;
        Routine rt;
        rt.name = row[0];
        rt.signature = row[1];
        rt.kind = row[2] == "PROCEDURE" ? RoutineKind::Procedure : RoutineKind::Function;
        rt.returnType = row[3];
        out.push_back(std::move(rt));
    }
    return out;
}

Table PostgresDatabase::describeTable(const std::string& tableName) {
    return describeTableInSchema(*this, "public", tableName);
}

std::vector<std::vector<std::string>>
PostgresDatabase::getTableData(const Table& table, int limit, int offset,
                               const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> data;
    std::string sql = "SELECT * FROM " + fullyQualified(table);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    if (!orderByClause.empty())
        sql += " ORDER BY " + orderByClause;
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);
    auto r = execute(sql, limit);
    if (!r.success() || r.empty())
        return data;
    return r[0].tableData;
}

std::vector<std::string> PostgresDatabase::getColumnNames(const Table& table) {
    std::vector<std::string> names;
    const std::string schema = table.schema.empty() ? "public" : table.schema;
    auto r = execute(std::format("SELECT column_name FROM information_schema.columns "
                                 "WHERE table_schema = '{}' AND table_name = '{}' "
                                 "ORDER BY ordinal_position",
                                 escapeLiteral(schema), escapeLiteral(table.name)),
                     10000);
    if (!r.success() || r.empty())
        return names;
    for (const auto& row : r[0].tableData)
        if (!row.empty())
            names.push_back(row[0]);
    return names;
}

int PostgresDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    std::string sql = "SELECT COUNT(*) FROM " + fullyQualified(table);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    auto r = execute(sql, 10);
    if (!r.success() || r.empty() || r[0].tableData.empty() || r[0].tableData[0].empty())
        return 0;
    try {
        return std::stoi(r[0].tableData[0][0]);
    } catch (...) {
        return 0;
    }
}

Status PostgresDatabase::createTable(const Table& table) {
    const std::string schema = table.schema.empty() ? "public" : table.schema;
    std::string sql = "CREATE TABLE " + quoteIdent(schema) + "." + quoteIdent(table.name) + " (";
    bool first = true;
    for (const auto& c : table.columns) {
        if (!first)
            sql += ", ";
        first = false;
        sql += quoteIdent(c.name) + " " + (c.type.empty() ? std::string("TEXT") : c.type);
        if (c.isPrimaryKey)
            sql += " PRIMARY KEY";
        if (c.isNotNull && !c.isPrimaryKey)
            sql += " NOT NULL";
        if (c.isUnique && !c.isPrimaryKey)
            sql += " UNIQUE";
        if (!c.defaultValue.empty())
            sql += " DEFAULT " + c.defaultValue;
    }
    for (const auto& fk : table.foreignKeys) {
        sql += std::format(", FOREIGN KEY ({}) REFERENCES {}({})", quoteIdent(fk.sourceColumn),
                           quoteIdent(fk.targetTable), quoteIdent(fk.targetColumn));
    }
    sql += ")";
    auto r = execute(sql, 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status PostgresDatabase::renameTable(const std::string& oldName, const std::string& newName) {
    auto r = execute(std::format(R"(ALTER TABLE "public"."{}" RENAME TO "{}")", oldName, newName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status PostgresDatabase::dropTable(const std::string& tableName) {
    auto r = execute(std::format(R"(DROP TABLE "public"."{}")", tableName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status PostgresDatabase::truncateTable(const std::string& tableName) {
    auto r = execute(std::format(R"(TRUNCATE TABLE ONLY "public"."{}")", tableName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status PostgresDatabase::dropColumn(const std::string& tableName, const std::string& columnName) {
    auto r = execute(
        std::format(R"(ALTER TABLE "public"."{}" DROP COLUMN "{}")", tableName, columnName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status PostgresDatabase::dropView(const std::string& viewName, bool isMaterialized) {
    const char* kw = isMaterialized ? "MATERIALIZED VIEW" : "VIEW";
    auto r = execute(std::format(R"(DROP {} "public"."{}")", kw, viewName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

// ---------- PostgresConnection impl (single connection cache) ----------

namespace {
class ConnectionImpl {
public:
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {
        if (info_.database.empty())
            info_.database = (info_.type == DatabaseType::REDSHIFT) ? "dev" : "postgres";
    }

    Status open() {
        try {
            auto db = std::dynamic_pointer_cast<PostgresDatabase>(database(info_.database));
            if (!db)
                return {false, "could not create default database handle"};
            (void)db->handle();
            open_ = true;
            return {true, ""};
        } catch (const std::exception& e) {
            open_ = false;
            return {false, e.what()};
        }
    }

    void close() {
        cache_.clear();
        open_ = false;
    }
    bool isOpen() const {
        return open_;
    }
    const ConnectionInfo& info() const {
        return info_;
    }

    std::vector<DatabasePtr> databases() {
        auto def = std::dynamic_pointer_cast<PostgresDatabase>(database(info_.database));
        if (!def)
            return {};
        auto r = def->execute("SELECT datname FROM pg_database WHERE datistemplate = false "
                              "ORDER BY datname",
                              10000);
        std::vector<DatabasePtr> out;
        if (!r.success() || r.empty())
            return out;
        for (const auto& row : r[0].tableData) {
            if (row.empty())
                continue;
            out.push_back(database(row[0]));
        }
        return out;
    }

    DatabasePtr database(const std::string& name) {
        const std::string n = name.empty() ? info_.database : name;
        auto it = cache_.find(n);
        if (it != cache_.end())
            return it->second;
        auto db = std::make_shared<PostgresDatabase>(info_, n);
        cache_[n] = db;
        return db;
    }

    Status createDatabase(const CreateDatabaseOptions& opts) {
        auto def = std::dynamic_pointer_cast<PostgresDatabase>(database(info_.database));
        if (!def)
            return {false, "no default connection"};
        std::string sql = "CREATE DATABASE " + quoteIdent(opts.name);
        if (!opts.owner.empty())
            sql += " OWNER " + quoteIdent(opts.owner);
        if (!opts.templateDb.empty())
            sql += " TEMPLATE " + quoteIdent(opts.templateDb);
        if (!opts.encoding.empty())
            sql += " ENCODING '" + escapeLiteral(opts.encoding) + "'";
        if (!opts.tablespace.empty())
            sql += " TABLESPACE " + quoteIdent(opts.tablespace);
        auto r = def->execute(sql, 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

    Status dropDatabase(const std::string& name) {
        auto def = std::dynamic_pointer_cast<PostgresDatabase>(database(info_.database));
        if (!def)
            return {false, "no default connection"};
        cache_.erase(name);
        def->execute(std::format("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                                 "WHERE datname = '{}' AND pid <> pg_backend_pid()",
                                 escapeLiteral(name)),
                     1000);
        auto r = def->execute("DROP DATABASE " + quoteIdent(name), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

    Status renameDatabase(const std::string& oldName, const std::string& newName) {
        auto def = std::dynamic_pointer_cast<PostgresDatabase>(database(info_.database));
        if (!def)
            return {false, "no default connection"};
        cache_.erase(oldName);
        auto r = def->execute(std::format(R"(ALTER DATABASE {} RENAME TO {})", quoteIdent(oldName),
                                          quoteIdent(newName)),
                              0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    ConnectionInfo info_;
    bool open_ = false;
    std::unordered_map<std::string, std::shared_ptr<PostgresDatabase>> cache_;
};
} // namespace

PostgresConnection::PostgresConnection(const ConnectionInfo& info) : info_(info) {
    impl_ = new ConnectionImpl(info);
}
PostgresConnection::~PostgresConnection() {
    delete static_cast<ConnectionImpl*>(impl_);
}
Status PostgresConnection::open() {
    return static_cast<ConnectionImpl*>(impl_)->open();
}
void PostgresConnection::close() {
    static_cast<ConnectionImpl*>(impl_)->close();
}
bool PostgresConnection::isOpen() const {
    return static_cast<const ConnectionImpl*>(impl_)->isOpen();
}
std::vector<DatabasePtr> PostgresConnection::databases() {
    return static_cast<ConnectionImpl*>(impl_)->databases();
}
DatabasePtr PostgresConnection::database(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->database(name);
}
Status PostgresConnection::createDatabase(const CreateDatabaseOptions& opts) {
    return static_cast<ConnectionImpl*>(impl_)->createDatabase(opts);
}
Status PostgresConnection::dropDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->dropDatabase(name);
}
Status PostgresConnection::renameDatabase(const std::string& oldName, const std::string& newName) {
    return static_cast<ConnectionImpl*>(impl_)->renameDatabase(oldName, newName);
}

} // namespace dearsql
