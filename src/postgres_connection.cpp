#include "dearsql/backends/postgres_connection.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iterator>
#include <libpq-fe.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

namespace dearsql {

namespace {

struct PgResultDeleter {
    void operator()(PGresult* r) const {
        if (r)
            PQclear(r);
    }
};
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

using Clock = std::chrono::high_resolution_clock;
double toMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

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

std::string literal(const std::string& s) {
    return "'" + escapeLiteral(s) + "'";
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

std::string literalList(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) {
        if (!out.empty())
            out += ", ";
        out += literal(n);
    }
    return out;
}

std::string maintenanceDatabase(DatabaseType type) {
    return type == DatabaseType::REDSHIFT ? "dev" : "postgres";
}

// block until the server's first response byte is readable: the boundary
// between server execution and data download
void waitForFirstByte(PGconn* conn) {
    const int sock = PQsocket(conn);
    if (sock < 0)
        return;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    select(sock + 1, &fds, nullptr, nullptr, nullptr);
}

// rowLimit <= 0 keeps every row
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
        const int limit = rowLimit > 0 ? std::min(nRows, rowLimit) : nRows;
        result.tableData.reserve(limit);
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
                    rowData.emplace_back(PQgetvalue(res, row, col),
                                         static_cast<size_t>(PQgetlength(res, row, col)));
                }
            }
            result.tableData.push_back(std::move(rowData));
        }
        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
        if (rowLimit > 0 && nRows > rowLimit)
            result.message += std::format(" (limited to {})", rowLimit);
    } else if (status == PGRES_COMMAND_OK) {
        const char* affected = PQcmdTuples(res);
        if (affected && *affected) {
            result.affectedRows = std::atoi(affected);
            result.message = std::format("{} row(s) affected", affected);
        } else {
            result.message = "Query executed successfully";
        }
    } else {
        result.success = false;
        result.errorMessage = PQresultErrorMessage(res);
    }
    return result;
}

StatementResult failure(std::string message) {
    StatementResult s;
    s.success = false;
    s.errorMessage = std::move(message);
    return s;
}

} // namespace

// ---------- PostgresSchema ----------

// one schema of a PostgresDatabase, on the database's connection. Unqualified
// names in execute() resolve here through search_path; the builder defaults
// qualify with schemaName().
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
    [[nodiscard]] std::string schemaName() const override {
        return name_;
    }
    bool alive() override {
        return parent_->alive();
    }
    void cancel() override {
        parent_->cancel();
    }

    std::vector<Table> tables() override {
        return parent_->tablesIn(name_);
    }
    std::vector<Table> views() override {
        return parent_->viewsIn(name_);
    }
    std::vector<Table> materializedViews() override {
        return parent_->materializedViewsIn(name_);
    }
    std::vector<std::string> sequences() override {
        return parent_->sequencesIn(name_);
    }
    std::vector<Routine> routines() override {
        return parent_->routinesIn(name_);
    }
    Table describeTable(const std::string& tableName) override {
        return parent_->describeIn(name_, tableName);
    }

    QueryResult execute(const std::string& sql, int rowLimit = 1000) override {
        return parent_->executeIn(name_, sql, rowLimit);
    }

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
                                                       const std::string& orderByClause) override {
        return IDatabase::getTableData(inSchema(table), limit, offset, whereClause, orderByClause);
    }
    std::vector<std::string> getColumnNames(const Table& table) override {
        return IDatabase::getColumnNames(inSchema(table));
    }
    int getRowCount(const Table& table, const std::string& whereClause) override {
        return IDatabase::getRowCount(inSchema(table), whereClause);
    }

    Status renameSchema(const std::string& newName) override {
        auto r = parent_->execute(
            std::format("ALTER SCHEMA {} RENAME TO {}", quoteIdent(name_), quoteIdent(newName)), 0);
        if (!r.success())
            return {false, r.errorMessage()};
        name_ = newName;
        return {true, ""};
    }
    Status dropSchema() override {
        auto r = parent_->execute(std::format("DROP SCHEMA {} CASCADE", quoteIdent(name_)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    Table inSchema(const Table& table) const {
        Table t = table;
        if (t.schema.empty())
            t.schema = name_;
        return t;
    }

    std::shared_ptr<PostgresDatabase> parent_;
    std::string name_;
};

// ---------- PostgresDatabase ----------

PostgresDatabase::PostgresDatabase(ConnectionInfo info, std::string dbName)
    : info_(std::move(info)), name_(std::move(dbName)) {}

PostgresDatabase::~PostgresDatabase() {
    {
        std::lock_guard lock(cancelMu_);
        if (cancel_)
            PQfreeCancel(cancel_);
        cancel_ = nullptr;
    }
    if (conn_)
        PQfinish(conn_);
}

void PostgresDatabase::ensureConn() {
    if (conn_)
        return;
    const auto connStr = info_.buildConnectionString(name_);
    PGconn* conn = PQconnectdb(connStr.c_str());
    if (PQstatus(conn) != CONNECTION_OK) {
        std::string err = PQerrorMessage(conn);
        PQfinish(conn);
        throw Error("PostgreSQL connection failed: " + err);
    }
    conn_ = conn;
    searchPath_.clear(); // a new session starts on the server default
    std::lock_guard lock(cancelMu_);
    cancel_ = PQgetCancel(conn_);
}

Status PostgresDatabase::open() {
    std::lock_guard lock(mu_);
    try {
        ensureConn();
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

bool PostgresDatabase::alive() {
    std::lock_guard lock(mu_);
    return !conn_ || PQstatus(conn_) == CONNECTION_OK;
}

void PostgresDatabase::cancel() {
    // a separate PGcancel object, so this never touches the busy PGconn
    std::lock_guard lock(cancelMu_);
    if (!cancel_)
        return;
    char err[256];
    PQcancel(cancel_, err, sizeof(err));
}

QueryResult PostgresDatabase::run(const std::string& sql, int rowLimit, bool timed) {
    QueryResult result;
    const auto startTime = Clock::now();
    std::lock_guard lock(mu_);
    try {
        ensureConn();
    } catch (const std::exception& e) {
        result.statements.push_back(failure(e.what()));
        return result;
    }

    if (timed) {
        // empty-query round trip ≈ network latency
        PgResultPtr ping(PQexec(conn_, ""));
    }
    const auto tPing = Clock::now();

    if (!PQsendQuery(conn_, sql.c_str())) {
        result.statements.push_back(failure(PQerrorMessage(conn_)));
        result.executionTimeMs = toMs(Clock::now() - startTime);
        return result;
    }
    if (timed)
        waitForFirstByte(conn_);
    const auto tExec = Clock::now();

    double downloadMs = 0.0;
    double parseMs = 0.0;
    for (;;) {
        const auto tDl = Clock::now();
        PGresult* raw = PQgetResult(conn_);
        downloadMs += toMs(Clock::now() - tDl);
        if (!raw)
            break;
        PgResultPtr res(raw);
        const auto tParse = Clock::now();
        auto r = extractPgResult(res.get(), rowLimit);
        parseMs += toMs(Clock::now() - tParse);
        if (r.success || !r.errorMessage.empty())
            result.statements.push_back(std::move(r));
    }

    // coarse client-side split; execution includes one-way latency
    if (timed) {
        result.phaseTimings = {{"network latency", toMs(tPing - startTime)},
                               {"execution", toMs(tExec - tPing)},
                               {"data download", downloadMs},
                               {"data parse", parseMs}};
    }
    result.executionTimeMs = toMs(Clock::now() - startTime);
    return result;
}

QueryResult PostgresDatabase::execute(const std::string& sql, int rowLimit) {
    return executeIn("", sql, rowLimit);
}

QueryResult PostgresDatabase::executeIn(const std::string& schema, const std::string& sql,
                                        int rowLimit) {
    // a name no schema can have: the path is unknown and must be set again
    static const std::string UNKNOWN(1, '\0');
    std::lock_guard lock(pathMu_);
    if (schema != searchPath_) {
        // its own round trip: inside the user's batch it would share an implicit
        // transaction, rolled back with a failing statement and fatal to VACUUM
        auto set = run(schema.empty() ? std::string("RESET search_path")
                                      : "SET search_path TO " + quoteIdent(schema),
                       0, false);
        if (!set.success())
            return set;
        searchPath_ = schema;
    }
    auto r = run(sql, rowLimit, true);
    std::string lower(sql.size(), ' ');
    std::ranges::transform(sql, lower.begin(), [](unsigned char c) { return std::tolower(c); });
    // a failure, or a batch that may have moved the path itself, leaves it unknown
    if (!r.success() || lower.find("search_path") != std::string::npos ||
        lower.find("rollback") != std::string::npos || lower.find("discard") != std::string::npos ||
        lower.find("reset") != std::string::npos)
        searchPath_ = UNKNOWN;
    return r;
}

std::vector<std::vector<std::string>> PostgresDatabase::rows(const std::string& sql) {
    auto r = run(sql, 0, false);
    if (!r.success())
        throw Error(r.errorMessage().empty() ? "query failed" : r.errorMessage());
    if (r.statements.empty())
        return {};
    return std::move(r.statements.front().tableData);
}

std::vector<std::string> PostgresDatabase::schemaNames() {
    std::vector<std::string> names;
    for (auto& row : rows("SELECT schema_name FROM information_schema.schemata "
                          "WHERE schema_name NOT IN ('information_schema', 'pg_catalog', 'pg_toast') "
                          "AND schema_name NOT LIKE 'pg\\_temp\\_%' "
                          "AND schema_name NOT LIKE 'pg\\_toast\\_temp\\_%' "
                          "ORDER BY schema_name")) {
        if (!row.empty())
            names.push_back(std::move(row[0]));
    }
    return names;
}

std::vector<DatabasePtr> PostgresDatabase::schemas() {
    std::vector<DatabasePtr> out;
    auto self = shared_from_this();
    for (const auto& n : schemaNames())
        out.push_back(std::make_shared<PostgresSchema>(self, n));
    return out;
}

DatabasePtr PostgresDatabase::schema(const std::string& schemaName) {
    return std::make_shared<PostgresSchema>(shared_from_this(), schemaName);
}

std::vector<Table> PostgresDatabase::tables() {
    std::vector<Table> out;
    for (const auto& s : schemaNames()) {
        auto list = tablesIn(s);
        std::ranges::move(list, std::back_inserter(out));
    }
    return out;
}

std::vector<Table> PostgresDatabase::views() {
    std::vector<Table> out;
    for (const auto& s : schemaNames()) {
        auto list = viewsIn(s);
        std::ranges::move(list, std::back_inserter(out));
    }
    return out;
}

std::vector<Table> PostgresDatabase::materializedViews() {
    std::vector<Table> out;
    for (const auto& s : schemaNames()) {
        auto list = materializedViewsIn(s);
        std::ranges::move(list, std::back_inserter(out));
    }
    return out;
}

std::vector<std::string> PostgresDatabase::sequences() {
    std::vector<std::string> out;
    for (const auto& s : schemaNames()) {
        for (const auto& seq : sequencesIn(s))
            out.push_back(s + "." + seq);
    }
    return out;
}

std::vector<Routine> PostgresDatabase::routines() {
    std::vector<Routine> out;
    for (const auto& s : schemaNames()) {
        auto list = routinesIn(s);
        std::ranges::move(list, std::back_inserter(out));
    }
    return out;
}

Table PostgresDatabase::describeTable(const std::string& tableName) {
    if (auto dot = tableName.find('.'); dot != std::string::npos)
        return describeIn(tableName.substr(0, dot), tableName.substr(dot + 1));
    auto current = rows("SELECT current_schema()");
    const std::string schema =
        current.empty() || current[0].empty() ? "public" : current[0][0];
    return describeIn(schema, tableName);
}

std::vector<Table> PostgresDatabase::tablesIn(const std::string& schema) {
    std::vector<std::string> names;
    for (auto& row : rows(std::format(
             "SELECT tablename FROM pg_tables WHERE schemaname = {} ORDER BY tablename",
             literal(schema)))) {
        if (!row.empty())
            names.push_back(std::move(row[0]));
    }
    if (names.empty())
        return {};
    const std::string nameList = literalList(names);

    std::unordered_map<std::string, std::vector<Column>> cols;
    for (const auto& row : rows(std::format(
             "SELECT c.table_name, c.column_name, c.data_type, c.is_nullable, "
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
             "WHERE c.table_schema = {} AND c.table_name IN ({}) "
             "ORDER BY c.table_name, c.ordinal_position",
             literal(schema), nameList))) {
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

    std::unordered_map<std::string, std::vector<ForeignKey>> fks;
    for (const auto& row : rows(std::format(
             "SELECT tc.table_name, kcu.column_name, ccu.table_name, ccu.column_name, "
             "tc.constraint_name "
             "FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu "
             "  ON tc.constraint_name = kcu.constraint_name "
             "  AND tc.table_schema = kcu.table_schema "
             "JOIN information_schema.constraint_column_usage ccu "
             "  ON ccu.constraint_name = tc.constraint_name "
             "  AND ccu.table_schema = tc.table_schema "
             "WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_schema = {} "
             "AND tc.table_name IN ({}) ORDER BY tc.table_name",
             literal(schema), nameList))) {
        if (row.size() < 5)
            continue;
        ForeignKey fk;
        fk.sourceColumn = row[1];
        fk.targetTable = row[2];
        fk.targetColumn = row[3];
        fk.name = row[4];
        fks[row[0]].push_back(std::move(fk));
    }

    // Redshift has no pg_total_relation_size; sizes are optional
    std::unordered_map<std::string, int64_t> sizes;
    auto sizeResult = run(std::format("SELECT c.relname, pg_total_relation_size(c.oid) "
                                      "FROM pg_catalog.pg_class c "
                                      "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
                                      "WHERE n.nspname = {} AND c.relkind = 'r' "
                                      "AND c.relname IN ({})",
                                      literal(schema), nameList),
                          0, false);
    if (sizeResult.success() && !sizeResult.empty()) {
        for (const auto& row : sizeResult[0].tableData) {
            if (row.size() < 2)
                continue;
            try {
                sizes[row[0]] = std::stoll(row[1]);
            } catch (...) {
                sizes[row[0]] = -1;
            }
        }
    }

    std::vector<Table> result;
    result.reserve(names.size());
    for (const auto& tn : names) {
        Table t;
        t.name = tn;
        t.schema = schema;
        t.fullName = schema + "." + tn;
        if (auto it = cols.find(tn); it != cols.end())
            t.columns = std::move(it->second);
        if (auto it = fks.find(tn); it != fks.end())
            t.foreignKeys = std::move(it->second);
        if (auto it = sizes.find(tn); it != sizes.end())
            t.sizeBytes = it->second;
        buildForeignKeyLookup(t);
        result.push_back(std::move(t));
    }
    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> PostgresDatabase::viewsIn(const std::string& schema) {
    std::vector<Table> result;
    for (auto& row : rows(std::format("SELECT viewname, COALESCE(definition, '') FROM pg_views "
                                      "WHERE schemaname = {} ORDER BY viewname",
                                      literal(schema)))) {
        if (row.size() < 2)
            continue;
        Table v;
        v.name = std::move(row[0]);
        v.schema = schema;
        v.fullName = schema + "." + v.name;
        v.definition = std::move(row[1]);
        result.push_back(std::move(v));
    }
    if (result.empty())
        return result;

    std::vector<std::string> names;
    for (const auto& v : result)
        names.push_back(v.name);
    std::unordered_map<std::string, std::vector<Column>> cols;
    for (const auto& row : rows(std::format("SELECT table_name, column_name, data_type, is_nullable "
                                            "FROM information_schema.columns "
                                            "WHERE table_schema = {} AND table_name IN ({}) "
                                            "ORDER BY table_name, ordinal_position",
                                            literal(schema), literalList(names)))) {
        if (row.size() < 4)
            continue;
        Column c;
        c.name = row[1];
        c.type = row[2];
        c.isNotNull = row[3] == "NO";
        cols[row[0]].push_back(std::move(c));
    }
    for (auto& v : result) {
        if (auto it = cols.find(v.name); it != cols.end())
            v.columns = std::move(it->second);
    }
    return result;
}

std::vector<Table> PostgresDatabase::materializedViewsIn(const std::string& schema) {
    std::vector<Table> result;
    for (auto& row : rows(std::format("SELECT matviewname, COALESCE(definition, '') "
                                      "FROM pg_matviews WHERE schemaname = {} "
                                      "ORDER BY matviewname",
                                      literal(schema)))) {
        if (row.size() < 2)
            continue;
        Table v;
        v.name = std::move(row[0]);
        v.schema = schema;
        v.fullName = schema + "." + v.name;
        v.definition = std::move(row[1]);
        result.push_back(std::move(v));
    }
    if (result.empty())
        return result;

    // information_schema.columns leaves matviews out
    std::vector<std::string> names;
    for (const auto& v : result)
        names.push_back(v.name);
    std::unordered_map<std::string, std::vector<Column>> cols;
    for (const auto& row :
         rows(std::format("SELECT c.relname, a.attname, "
                          "pg_catalog.format_type(a.atttypid, a.atttypmod), a.attnotnull "
                          "FROM pg_catalog.pg_attribute a "
                          "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
                          "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
                          "WHERE n.nspname = {} AND c.relkind = 'm' "
                          "AND a.attnum > 0 AND NOT a.attisdropped AND c.relname IN ({}) "
                          "ORDER BY c.relname, a.attnum",
                          literal(schema), literalList(names)))) {
        if (row.size() < 4)
            continue;
        Column c;
        c.name = row[1];
        c.type = row[2];
        c.isNotNull = row[3] == BOOL_TRUE_SENTINEL;
        cols[row[0]].push_back(std::move(c));
    }
    for (auto& v : result) {
        if (auto it = cols.find(v.name); it != cols.end())
            v.columns = std::move(it->second);
    }
    return result;
}

std::vector<std::string> PostgresDatabase::sequencesIn(const std::string& schema) {
    std::vector<std::string> out;
    for (auto& row : rows(std::format("SELECT sequencename FROM pg_sequences "
                                      "WHERE schemaname = {} ORDER BY sequencename",
                                      literal(schema)))) {
        if (!row.empty())
            out.push_back(std::move(row[0]));
    }
    return out;
}

std::vector<Routine> PostgresDatabase::routinesIn(const std::string& schema) {
    std::vector<Routine> out;
    for (auto& row : rows(std::format(
             "SELECT p.proname, "
             "p.proname || '(' || "
             "COALESCE(pg_catalog.pg_get_function_identity_arguments(p.oid), '') || ')', "
             "CASE p.prokind WHEN 'f' THEN 'FUNCTION' ELSE 'PROCEDURE' END, "
             "pg_catalog.pg_get_function_result(p.oid) "
             "FROM pg_catalog.pg_proc p "
             "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
             "WHERE n.nspname = {} AND p.prokind IN ('f', 'p') ORDER BY p.prokind, p.proname",
             literal(schema)))) {
        if (row.size() < 4)
            continue;
        Routine rt;
        rt.name = std::move(row[0]);
        rt.signature = std::move(row[1]);
        rt.kind = row[2] == "PROCEDURE" ? RoutineKind::Procedure : RoutineKind::Function;
        rt.returnType = row[3] == NULL_SENTINEL ? "" : std::move(row[3]);
        out.push_back(std::move(rt));
    }
    return out;
}

Table PostgresDatabase::describeIn(const std::string& schema, const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.schema = schema;
    t.fullName = schema + "." + tableName;
    const std::string schemaLit = literal(schema);
    const std::string tableLit = literal(tableName);
    const std::string regclass = literal(quoteIdent(schema) + "." + quoteIdent(tableName));

    for (const auto& row : rows(std::format(
             "SELECT column_name, data_type, is_nullable, COALESCE(column_default, ''), "
             "CASE WHEN column_default LIKE 'nextval(%' OR is_identity = 'YES' "
             "  THEN 'true' ELSE 'false' END "
             "FROM information_schema.columns "
             "WHERE table_schema = {} AND table_name = {} ORDER BY ordinal_position",
             schemaLit, tableLit))) {
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
    if (t.columns.empty())
        return t;

    std::vector<std::string> pkCols;
    for (auto& row : rows(std::format(
             "SELECT a.attname FROM pg_index i "
             "JOIN pg_attribute a ON a.attrelid = i.indrelid AND a.attnum = ANY(i.indkey) "
             "WHERE i.indrelid = {}::regclass AND i.indisprimary",
             regclass))) {
        if (!row.empty())
            pkCols.push_back(std::move(row[0]));
    }
    for (auto& c : t.columns)
        c.isPrimaryKey = std::ranges::find(pkCols, c.name) != pkCols.end();

    for (const auto& row : rows(std::format(
             "SELECT i.relname, a.attname, ix.indisunique "
             "FROM pg_class t "
             "JOIN pg_namespace n ON n.oid = t.relnamespace "
             "JOIN pg_index ix ON t.oid = ix.indrelid "
             "JOIN pg_class i ON i.oid = ix.indexrelid "
             "JOIN pg_attribute a ON a.attrelid = t.oid AND a.attnum = ANY(ix.indkey) "
             "WHERE t.relkind = 'r' AND n.nspname = {} AND t.relname = {} "
             "AND NOT ix.indisprimary ORDER BY i.relname, a.attnum",
             schemaLit, tableLit))) {
        if (row.size() < 3)
            continue;
        auto it = std::ranges::find(t.indexes, row[0], &Index::name);
        if (it == t.indexes.end()) {
            Index idx;
            idx.name = row[0];
            idx.isUnique = row[2] == BOOL_TRUE_SENTINEL;
            t.indexes.push_back(std::move(idx));
            it = std::prev(t.indexes.end());
        }
        it->columns.push_back(row[1]);
    }

    for (const auto& row : rows(std::format(
             "SELECT kcu.column_name, ccu.table_name, ccu.column_name, tc.constraint_name "
             "FROM information_schema.table_constraints tc "
             "JOIN information_schema.key_column_usage kcu "
             "  ON tc.constraint_name = kcu.constraint_name "
             "  AND tc.table_schema = kcu.table_schema "
             "JOIN information_schema.constraint_column_usage ccu "
             "  ON ccu.constraint_name = tc.constraint_name "
             "  AND ccu.table_schema = tc.table_schema "
             "WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_schema = {} "
             "AND tc.table_name = {}",
             schemaLit, tableLit))) {
        if (row.size() < 4)
            continue;
        ForeignKey fk;
        fk.sourceColumn = row[0];
        fk.targetTable = row[1];
        fk.targetColumn = row[2];
        fk.name = row[3];
        t.foreignKeys.push_back(std::move(fk));
    }
    buildForeignKeyLookup(t);

    // Redshift has no pg_total_relation_size; size is optional
    auto size = run(std::format("SELECT pg_total_relation_size({}::regclass)", regclass), 1, false);
    if (size.success() && !size.empty() && !size[0].tableData.empty() &&
        !size[0].tableData[0].empty() && size[0].tableData[0][0] != NULL_SENTINEL) {
        try {
            t.sizeBytes = std::stoll(size[0].tableData[0][0]);
        } catch (...) {
            t.sizeBytes = -1;
        }
    }
    return t;
}

// ---------- PostgresConnection ----------

namespace {

// Caches one PostgresDatabase per database name.
class ConnectionImpl {
public:
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {
        if (info_.database.empty())
            info_.database = maintenanceDatabase(info_.type);
    }

    Status open() {
        auto st = defaultDb()->open();
        open_ = st.first;
        return st;
    }

    void close() {
        std::lock_guard lock(mu_);
        cache_.clear();
        open_ = false;
    }
    bool isOpen() const {
        return open_;
    }

    std::vector<DatabasePtr> databases() {
        std::vector<DatabasePtr> out;
        auto r = defaultDb()->execute(
            "SELECT datname FROM pg_database WHERE datistemplate = false ORDER BY datname", 0);
        if (!r.success())
            throw Error(r.errorMessage());
        if (r.empty())
            return out;
        for (const auto& row : r[0].tableData) {
            if (!row.empty())
                out.push_back(database(row[0]));
        }
        return out;
    }

    DatabasePtr database(const std::string& name) {
        std::lock_guard lock(mu_);
        const std::string n = name.empty() ? info_.database : name;
        auto it = cache_.find(n);
        if (it != cache_.end())
            return it->second;
        auto db = std::make_shared<PostgresDatabase>(info_, n);
        cache_[n] = db;
        return db;
    }

    DatabasePtr openDatabase(const std::string& name) {
        const ConnectionInfo info = snapshot();
        return std::make_shared<PostgresDatabase>(info, name.empty() ? info.database : name);
    }

    Status createDatabase(const CreateDatabaseOptions& opts) {
        if (opts.name.empty())
            return {false, "Database name cannot be empty"};
        std::string sql = "CREATE DATABASE " + quoteIdent(opts.name);
        if (!opts.owner.empty())
            sql += " OWNER " + quoteIdent(opts.owner);
        if (!opts.templateDb.empty())
            sql += " TEMPLATE " + quoteIdent(opts.templateDb);
        if (!opts.encoding.empty())
            sql += " ENCODING " + literal(opts.encoding);
        if (!opts.tablespace.empty())
            sql += " TABLESPACE " + quoteIdent(opts.tablespace);
        auto db = defaultDb();
        auto r = db->execute(sql, 0);
        if (!r.success())
            return {false, r.errorMessage()};
        if (!opts.comment.empty()) {
            auto c = db->execute(std::format("COMMENT ON DATABASE {} IS {}", quoteIdent(opts.name),
                                             literal(opts.comment)),
                                 0);
            if (!c.success())
                return {true,
                        std::format("Created database, but failed to set comment: {}",
                                    c.errorMessage())};
        }
        return {true, ""};
    }

    Status dropDatabase(const std::string& name) {
        // a session cannot drop its own database: go through the maintenance one
        const ConnectionInfo info = snapshot();
        const bool connectedDb = name == info.database;
        {
            std::lock_guard lock(mu_);
            cache_.erase(name);
        }
        std::shared_ptr<PostgresDatabase> via = defaultDb();
        if (connectedDb) {
            via = std::make_shared<PostgresDatabase>(info, maintenanceDatabase(info.type));
            if (auto st = via->open(); !st.first)
                return {false,
                        std::format("Failed to connect to maintenance database: {}", st.second)};
        }
        via->execute(std::format("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
                                 "WHERE datname = {} AND pid <> pg_backend_pid()",
                                 literal(name)),
                     0);
        auto r = via->execute("DROP DATABASE " + quoteIdent(name), 0);
        if (!r.success())
            return {false, r.errorMessage()};
        if (connectedDb) {
            std::lock_guard lock(mu_);
            info_.database = via->name();
            cache_[info_.database] = via;
        }
        return {true, ""};
    }

    Status renameDatabase(const std::string& oldName, const std::string& newName) {
        {
            std::lock_guard lock(mu_);
            cache_.erase(oldName);
        }
        auto r = defaultDb()->execute(
            std::format("ALTER DATABASE {} RENAME TO {}", quoteIdent(oldName), quoteIdent(newName)),
            0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    std::shared_ptr<PostgresDatabase> defaultDb() {
        return std::static_pointer_cast<PostgresDatabase>(database(""));
    }
    ConnectionInfo snapshot() const {
        std::lock_guard lock(mu_);
        return info_;
    }

    // cache_ and info_ are reached from loader, ui, refresh and agent threads
    mutable std::mutex mu_;
    ConnectionInfo info_;
    std::atomic<bool> open_ = false;
    std::unordered_map<std::string, DatabasePtr> cache_;
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
DatabasePtr PostgresConnection::openDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->openDatabase(name);
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
