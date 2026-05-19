#include "dearsql/backends/mssql_connection.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sybdb.h>
#include <unordered_map>
#include <vector>

namespace dearsql {

namespace {

// thread-local error string captured by db-lib error/message handlers
thread_local std::string tls_lastError;

int dblibErrorHandler(DBPROCESS* /*dbproc*/, int /*severity*/, int /*dberr*/, int /*oserr*/,
                      char* dberrstr, char* oserrstr) {
    std::string msg;
    if (dberrstr)
        msg = dberrstr;
    if (oserrstr && oserrstr[0]) {
        if (!msg.empty())
            msg += "; ";
        msg += oserrstr;
    }
    tls_lastError = msg;
    return INT_CANCEL;
}

int dblibMessageHandler(DBPROCESS* /*dbproc*/, DBINT /*msgno*/, int /*msgstate*/, int severity,
                        char* msgtext, char* /*srvname*/, char* /*procname*/, int /*line*/) {
    if (severity > 10 && msgtext) {
        tls_lastError = msgtext;
    }
    return 0;
}

std::once_flag g_dbLibInitFlag;

void initDbLib() {
    std::call_once(g_dbLibInitFlag, []() {
        dbinit();
        dberrhandle(dblibErrorHandler);
        dbmsghandle(dblibMessageHandler);
    });
}

void clearLastError() {
    tls_lastError.clear();
}

std::string getLastError() {
    return tls_lastError.empty() ? "Unknown error" : tls_lastError;
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

std::string quoteIdent(const std::string& id) {
    std::string out = "[";
    out.reserve(id.size() + 2);
    for (char c : id) {
        if (c == ']')
            out += ']';
        out += c;
    }
    out += ']';
    return out;
}

std::string qualifiedRef(const std::string& schema, const std::string& name) {
    if (schema.empty())
        return quoteIdent(name);
    return quoteIdent(schema) + "." + quoteIdent(name);
}

std::string colToString(DBPROCESS* dbproc, int col) {
    BYTE* data = dbdata(dbproc, col);
    int len = dbdatlen(dbproc, col);
    if (!data || len < 0)
        return std::string(NULL_SENTINEL);

    int type = dbcoltype(dbproc, col);

    char buf[8192];
    DBINT converted = dbconvert(dbproc, type, data, len, SYBCHAR,
                                reinterpret_cast<BYTE*>(buf), sizeof(buf) - 1);
    if (converted >= 0) {
        buf[converted] = '\0';
        return std::string(buf);
    }
    return std::string(reinterpret_cast<char*>(data), len);
}

int colToInt(DBPROCESS* dbproc, int col) {
    BYTE* data = dbdata(dbproc, col);
    int len = dbdatlen(dbproc, col);
    if (!data || len < 0)
        return 0;
    int type = dbcoltype(dbproc, col);
    DBINT val = 0;
    dbconvert(dbproc, type, data, len, SYBINT4, reinterpret_cast<BYTE*>(&val), sizeof(val));
    return static_cast<int>(val);
}

bool execQuery(DBPROCESS* dbproc, const std::string& sql) {
    clearLastError();
    dbcmd(dbproc, sql.c_str());
    return dbsqlexec(dbproc) == SUCCEED;
}

void drainResults(DBPROCESS* dbproc) {
    while (dbresults(dbproc) != NO_MORE_RESULTS) {
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
        }
    }
}

StatementResult extractDbLibResult(DBPROCESS* dbproc, int rowLimit) {
    StatementResult result;
    int numCols = dbnumcols(dbproc);

    if (numCols > 0) {
        for (int i = 1; i <= numCols; i++) {
            result.columnNames.emplace_back(dbcolname(dbproc, i));
        }
        int rowCount = 0;
        STATUS rowCode;
        while ((rowCode = dbnextrow(dbproc)) != NO_MORE_ROWS && rowCount < rowLimit) {
            if (rowCode == FAIL)
                break;
            std::vector<std::string> rowData;
            rowData.reserve(numCols);
            for (int i = 1; i <= numCols; i++) {
                rowData.push_back(colToString(dbproc, i));
            }
            result.tableData.push_back(std::move(rowData));
            rowCount++;
        }
        // drain any remaining rows so dblib state stays clean
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
        }
        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
        if (static_cast<int>(result.tableData.size()) >= rowLimit) {
            result.message += std::format(" (limited to {})", rowLimit);
        }
        result.success = true;
    } else {
        DBINT affected = DBCOUNT(dbproc);
        if (affected >= 0) {
            result.message = std::format("{} row(s) affected", affected);
            result.affectedRows = static_cast<int>(affected);
        } else {
            result.message = "Query executed successfully";
        }
        result.success = true;
    }
    return result;
}

QueryResult executeQueryOnProcess(DBPROCESS* dbproc, const std::string& query, int rowLimit) {
    QueryResult result;
    clearLastError();
    dbcmd(dbproc, query.c_str());

    if (dbsqlexec(dbproc) == FAIL) {
        StatementResult r;
        r.success = false;
        r.errorMessage = getLastError();
        result.statements.push_back(std::move(r));
        dbcancel(dbproc);
        return result;
    }

    RETCODE rc;
    while ((rc = dbresults(dbproc)) != NO_MORE_RESULTS) {
        if (rc == FAIL) {
            StatementResult r;
            r.success = false;
            r.errorMessage = getLastError();
            result.statements.push_back(std::move(r));
            break;
        }
        auto r = extractDbLibResult(dbproc, rowLimit);
        if (r.success || !r.errorMessage.empty()) {
            result.statements.push_back(std::move(r));
        }
    }
    return result;
}

DBPROCESS* openDbLibConnection(const ConnectionInfo& info, const std::string& dbName) {
    LOGINREC* login = dblogin();
    if (!login)
        throw std::runtime_error("dblogin() failed");

    DBSETLUSER(login, info.username.c_str());
    DBSETLPWD(login, info.password.c_str());
    DBSETLAPP(login, "DearSQL");
    dbsetlversion(login, DBVERSION_73);

    if (info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
        info.sslmode == SslMode::VerifyFull) {
        DBSETLENCRYPT(login, TRUE);
    }

    dbsetlogintime(10);

    std::string serverStr = info.host + ":" + std::to_string(info.port);
    clearLastError();
    DBPROCESS* dbproc = dbopen(login, serverStr.c_str());
    dbloginfree(login);
    if (!dbproc) {
        throw std::runtime_error("MSSQL connection failed: " + getLastError());
    }

    const std::string targetDb = !dbName.empty() ? dbName : info.database;
    if (!targetDb.empty()) {
        clearLastError();
        if (dbuse(dbproc, targetDb.c_str()) != SUCCEED) {
            std::string err = getLastError();
            dbclose(dbproc);
            throw std::runtime_error("MSSQL dbuse failed: " + err);
        }
    }
    return dbproc;
}

// RAII for a temp DBPROCESS (used for cross-database admin work)
struct DbProcessGuard {
    DBPROCESS* proc = nullptr;
    explicit DbProcessGuard(DBPROCESS* p) : proc(p) {}
    ~DbProcessGuard() {
        if (proc)
            dbclose(proc);
    }
    DbProcessGuard(const DbProcessGuard&) = delete;
    DbProcessGuard& operator=(const DbProcessGuard&) = delete;
    DBPROCESS* get() const {
        return proc;
    }
};

class MSSQLDatabase;

// per-database operations using a MSSQLDatabase's session.
std::vector<std::string> loadSchemaNames(MSSQLDatabase& db);
std::vector<Table> loadTablesForSchema(MSSQLDatabase& db, const std::string& schema);
std::vector<Table> loadViewsForSchema(MSSQLDatabase& db, const std::string& schema);
std::vector<Routine> loadRoutinesForSchema(MSSQLDatabase& db, const std::string& schema);
Table describeTableInSchema(MSSQLDatabase& db, const std::string& schema,
                            const std::string& tableName);

// A single database (catalog) on the MSSQL server. Owns one DBPROCESS pinned
// to that database. enable_shared_from_this so schemas() can hand a parent
// pointer to MSSQLSchema instances.
class MSSQLDatabase final : public IDatabase,
                            public std::enable_shared_from_this<MSSQLDatabase> {
public:
    MSSQLDatabase(ConnectionInfo info, std::string dbName)
        : info_(std::move(info)), name_(std::move(dbName)) {}

    ~MSSQLDatabase() override {
        if (conn_)
            dbclose(conn_);
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

    // exposed so the connection impl can lazily verify the handle.
    DBPROCESS* handle() {
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
        initDbLib();
        conn_ = openDbLibConnection(info_, name_);
    }

    std::string fullyQualified(const Table& t) const {
        const std::string s = t.schema.empty() ? "dbo" : t.schema;
        return quoteIdent(s) + "." + quoteIdent(t.name);
    }

    ConnectionInfo info_;
    std::string name_;
    DBPROCESS* conn_ = nullptr;
    std::mutex mu_;
};

// A single schema (e.g. "dbo") inside a MSSQLDatabase. Holds a shared_ptr to
// its parent database so the underlying DBPROCESS outlives the schema view.
class MSSQLSchema final : public IDatabase {
public:
    MSSQLSchema(std::shared_ptr<MSSQLDatabase> parent, std::string schemaName)
        : parent_(std::move(parent)), name_(std::move(schemaName)) {}

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return parent_->type();
    }

    std::vector<Table> tables() override {
        return loadTablesForSchema(*parent_, name_);
    }
    std::vector<Table> views() override {
        return loadViewsForSchema(*parent_, name_);
    }
    std::vector<Routine> routines() override {
        return loadRoutinesForSchema(*parent_, name_);
    }
    Table describeTable(const std::string& tableName) override {
        return describeTableInSchema(*parent_, name_, tableName);
    }

    QueryResult execute(const std::string& sql, int rowLimit) override {
        // MSSQL has no per-session schema-search-path equivalent; just defer
        // to the parent database. The caller is expected to fully qualify or
        // rely on dbo as the user's default.
        return parent_->execute(sql, rowLimit);
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
        // sp_rename expects un-bracketed args
        auto r = parent_->execute(std::format("EXEC sp_rename '{}.{}', '{}'", name_, oldName,
                                              newName),
                                  0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status dropTable(const std::string& tableName) override {
        auto r = parent_->execute(
            std::format("DROP TABLE {}.{}", quoteIdent(name_), quoteIdent(tableName)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status truncateTable(const std::string& tableName) override {
        auto r = parent_->execute(
            std::format("TRUNCATE TABLE {}.{}", quoteIdent(name_), quoteIdent(tableName)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status dropColumn(const std::string& tableName, const std::string& columnName) override {
        auto r = parent_->execute(std::format("ALTER TABLE {}.{} DROP COLUMN {}",
                                              quoteIdent(name_), quoteIdent(tableName),
                                              quoteIdent(columnName)),
                                  0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status dropView(const std::string& viewName, bool /*isMaterialized*/) override {
        auto r = parent_->execute(
            std::format("DROP VIEW {}.{}", quoteIdent(name_), quoteIdent(viewName)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }
    Status renameSchema(const std::string& newName) override {
        // MSSQL has no rename-schema DDL; the canonical approach is to create
        // a new schema and transfer objects. We surface this as unsupported.
        (void)newName;
        return {false, "renameSchema not supported on MSSQL"};
    }
    Status dropSchema() override {
        auto r = parent_->execute(std::format("DROP SCHEMA {}", quoteIdent(name_)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    std::shared_ptr<MSSQLDatabase> parent_;
    std::string name_;
};

// ---------- helpers using MSSQLDatabase ----------

std::vector<std::string> loadSchemaNames(MSSQLDatabase& db) {
    std::vector<std::string> out;
    auto r = db.execute("SELECT SCHEMA_NAME FROM INFORMATION_SCHEMA.SCHEMATA "
                        "WHERE CATALOG_NAME = DB_NAME() "
                        "AND SCHEMA_NAME NOT IN ('INFORMATION_SCHEMA', 'sys', "
                        "'db_owner', 'db_accessadmin', 'db_securityadmin', "
                        "'db_ddladmin', 'db_backupoperator', 'db_datareader', "
                        "'db_datawriter', 'db_denydatareader', 'db_denydatawriter') "
                        "ORDER BY SCHEMA_NAME",
                        10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData)
        if (!row.empty() && !isNullSentinel(row[0]))
            out.push_back(row[0]);
    return out;
}

std::vector<Table> loadTablesForSchema(MSSQLDatabase& db, const std::string& schema) {
    std::vector<Table> result;

    auto namesResult = db.execute(
        std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
                    "WHERE TABLE_TYPE = 'BASE TABLE' AND TABLE_CATALOG = DB_NAME() "
                    "AND TABLE_SCHEMA = '{}' ORDER BY TABLE_NAME",
                    escapeLiteral(schema)),
        10000);
    if (!namesResult.success() || namesResult.empty())
        return result;

    std::vector<std::string> tableNames;
    for (const auto& row : namesResult[0].tableData)
        if (!row.empty())
            tableNames.push_back(row[0]);
    if (tableNames.empty())
        return result;

    std::string inClause;
    for (size_t i = 0; i < tableNames.size(); ++i) {
        if (i)
            inClause += ", ";
        inClause += "'" + escapeLiteral(tableNames[i]) + "'";
    }

    // bulk columns
    std::map<std::string, std::vector<Column>> tableColumns;
    {
        auto r = db.execute(
            std::format(
                "SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE, "
                "COLUMNPROPERTY(OBJECT_ID('{0}.' + TABLE_NAME), COLUMN_NAME, 'IsIdentity'), "
                "ISNULL(COLUMN_DEFAULT, '') "
                "FROM INFORMATION_SCHEMA.COLUMNS "
                "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                "AND TABLE_NAME IN ({1}) ORDER BY TABLE_NAME, ORDINAL_POSITION",
                escapeLiteral(schema), inClause),
            100000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 6)
                    continue;
                Column col;
                col.name = row[1];
                col.type = row[2];
                col.isNotNull = row[3] == "NO";
                col.isAutoIncrement = row[4] == "1";
                col.defaultValue = row[5];
                tableColumns[row[0]].push_back(std::move(col));
            }
        }
    }

    // bulk primary keys
    {
        auto r = db.execute(
            std::format("SELECT tc.TABLE_NAME, c.COLUMN_NAME "
                        "FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
                        "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE c "
                        "ON c.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                        "AND c.TABLE_SCHEMA = tc.TABLE_SCHEMA "
                        "WHERE tc.TABLE_SCHEMA = '{0}' AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY' "
                        "AND tc.TABLE_NAME IN ({1})",
                        escapeLiteral(schema), inClause),
            100000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 2)
                    continue;
                auto it = tableColumns.find(row[0]);
                if (it == tableColumns.end())
                    continue;
                for (auto& c : it->second)
                    if (c.name == row[1])
                        c.isPrimaryKey = true;
            }
        }
    }

    // bulk foreign keys
    std::map<std::string, std::vector<ForeignKey>> tableFks;
    {
        auto r = db.execute(
            std::format("SELECT OBJECT_NAME(fk.parent_object_id), fk.name, "
                        "COL_NAME(fkc.parent_object_id, fkc.parent_column_id), "
                        "OBJECT_NAME(fkc.referenced_object_id), "
                        "COL_NAME(fkc.referenced_object_id, fkc.referenced_column_id) "
                        "FROM sys.foreign_keys fk "
                        "JOIN sys.foreign_key_columns fkc "
                        "ON fk.object_id = fkc.constraint_object_id "
                        "WHERE OBJECT_SCHEMA_NAME(fk.parent_object_id) = '{}'",
                        escapeLiteral(schema)),
            100000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 5)
                    continue;
                ForeignKey fk;
                fk.name = row[1];
                fk.sourceColumn = row[2];
                fk.targetTable = row[3];
                fk.targetColumn = row[4];
                tableFks[row[0]].push_back(std::move(fk));
            }
        }
    }

    // bulk indexes (non-PK)
    std::map<std::string, std::vector<Index>> tableIndexes;
    {
        auto r = db.execute(
            std::format("SELECT OBJECT_NAME(i.object_id), i.name, i.is_unique, c.name "
                        "FROM sys.indexes i "
                        "JOIN sys.index_columns ic ON i.object_id = ic.object_id "
                        "AND i.index_id = ic.index_id "
                        "JOIN sys.columns c ON ic.object_id = c.object_id "
                        "AND ic.column_id = c.column_id "
                        "WHERE i.is_primary_key = 0 AND i.type > 0 "
                        "AND OBJECT_SCHEMA_NAME(i.object_id) = '{}' "
                        "ORDER BY OBJECT_NAME(i.object_id), i.name, ic.key_ordinal",
                        escapeLiteral(schema)),
            100000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 4)
                    continue;
                auto& vec = tableIndexes[row[0]];
                if (vec.empty() || vec.back().name != row[1]) {
                    Index idx;
                    idx.name = row[1];
                    idx.isUnique = (row[2] == "1");
                    idx.columns.push_back(row[3]);
                    vec.push_back(std::move(idx));
                } else {
                    vec.back().columns.push_back(row[3]);
                }
            }
        }
    }

    for (const auto& tn : tableNames) {
        Table t;
        t.name = tn;
        t.schema = schema;
        t.fullName = schema + "." + tn;
        if (auto it = tableColumns.find(tn); it != tableColumns.end())
            t.columns = std::move(it->second);
        if (auto it = tableFks.find(tn); it != tableFks.end())
            t.foreignKeys = std::move(it->second);
        if (auto it = tableIndexes.find(tn); it != tableIndexes.end())
            t.indexes = std::move(it->second);
        buildForeignKeyLookup(t);
        result.push_back(std::move(t));
    }
    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> loadViewsForSchema(MSSQLDatabase& db, const std::string& schema) {
    std::vector<Table> result;
    auto names = db.execute(
        std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.VIEWS "
                    "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                    "ORDER BY TABLE_NAME",
                    escapeLiteral(schema)),
        10000);
    if (!names.success() || names.empty())
        return result;
    std::vector<std::string> viewNames;
    for (const auto& row : names[0].tableData)
        if (!row.empty())
            viewNames.push_back(row[0]);
    if (viewNames.empty())
        return result;

    std::string inClause;
    for (size_t i = 0; i < viewNames.size(); ++i) {
        if (i)
            inClause += ", ";
        inClause += "'" + escapeLiteral(viewNames[i]) + "'";
    }

    std::map<std::string, std::vector<Column>> viewColumns;
    {
        auto r = db.execute(
            std::format("SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE "
                        "FROM INFORMATION_SCHEMA.COLUMNS "
                        "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                        "AND TABLE_NAME IN ({1}) ORDER BY TABLE_NAME, ORDINAL_POSITION",
                        escapeLiteral(schema), inClause),
            100000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 4)
                    continue;
                Column col;
                col.name = row[1];
                col.type = row[2];
                col.isNotNull = row[3] == "NO";
                viewColumns[row[0]].push_back(std::move(col));
            }
        }
    }

    for (const auto& vn : viewNames) {
        Table v;
        v.name = vn;
        v.schema = schema;
        v.fullName = schema + "." + vn;
        if (auto it = viewColumns.find(vn); it != viewColumns.end())
            v.columns = std::move(it->second);
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<Routine> loadRoutinesForSchema(MSSQLDatabase& db, const std::string& schema) {
    std::vector<Routine> out;
    auto r = db.execute(
        std::format(
            "SELECT o.name, "
            "o.name + '(' + ISNULL(STUFF((SELECT ', ' + p.name + ' ' + TYPE_NAME(p.user_type_id) "
            "FROM sys.parameters p WHERE p.object_id = o.object_id "
            "ORDER BY p.parameter_id FOR XML PATH('')), 1, 2, ''), '') + ')' AS signature, "
            "CASE o.type WHEN 'P' THEN 'PROCEDURE' WHEN 'FN' THEN 'FUNCTION' "
            "WHEN 'IF' THEN 'FUNCTION' WHEN 'TF' THEN 'FUNCTION' END AS kind, "
            "ISNULL(TYPE_NAME(o.type_desc), '') AS return_type "
            "FROM sys.objects o "
            "WHERE o.schema_id = SCHEMA_ID('{}') "
            "AND o.type IN ('P', 'FN', 'IF', 'TF') "
            "ORDER BY o.type, o.name",
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
        rt.kind = (row[2] == "PROCEDURE") ? RoutineKind::Procedure : RoutineKind::Function;
        rt.returnType = row[3];
        out.push_back(std::move(rt));
    }
    return out;
}

Table describeTableInSchema(MSSQLDatabase& db, const std::string& schema,
                            const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.schema = schema;
    t.fullName = schema + "." + tableName;

    auto cols = db.execute(
        std::format("SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE, "
                    "ISNULL(COLUMN_DEFAULT, ''), "
                    "COLUMNPROPERTY(OBJECT_ID('{0}.{1}'), COLUMN_NAME, 'IsIdentity') "
                    "FROM INFORMATION_SCHEMA.COLUMNS "
                    "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                    "AND TABLE_NAME = '{1}' ORDER BY ORDINAL_POSITION",
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
            c.isAutoIncrement = row[4] == "1";
            t.columns.push_back(std::move(c));
        }
    }

    auto pk = db.execute(
        std::format("SELECT c.COLUMN_NAME FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
                    "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE c "
                    "ON c.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                    "AND c.TABLE_SCHEMA = tc.TABLE_SCHEMA "
                    "WHERE tc.TABLE_SCHEMA = '{}' AND tc.TABLE_NAME = '{}' "
                    "AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY'",
                    escapeLiteral(schema), escapeLiteral(tableName)),
        10000);
    if (pk.success() && !pk.empty()) {
        for (const auto& row : pk[0].tableData) {
            if (row.empty())
                continue;
            for (auto& c : t.columns)
                if (c.name == row[0])
                    c.isPrimaryKey = true;
        }
    }

    auto fks = db.execute(
        std::format("SELECT fk.name, "
                    "COL_NAME(fkc.parent_object_id, fkc.parent_column_id), "
                    "OBJECT_NAME(fkc.referenced_object_id), "
                    "COL_NAME(fkc.referenced_object_id, fkc.referenced_column_id) "
                    "FROM sys.foreign_keys fk "
                    "JOIN sys.foreign_key_columns fkc "
                    "ON fk.object_id = fkc.constraint_object_id "
                    "WHERE OBJECT_SCHEMA_NAME(fk.parent_object_id) = '{}' "
                    "AND OBJECT_NAME(fk.parent_object_id) = '{}'",
                    escapeLiteral(schema), escapeLiteral(tableName)),
        10000);
    if (fks.success() && !fks.empty()) {
        for (const auto& row : fks[0].tableData) {
            if (row.size() < 4)
                continue;
            ForeignKey fk;
            fk.name = row[0];
            fk.sourceColumn = row[1];
            fk.targetTable = row[2];
            fk.targetColumn = row[3];
            t.foreignKeys.push_back(std::move(fk));
        }
    }

    auto idxs = db.execute(
        std::format("SELECT i.name, i.is_unique, c.name "
                    "FROM sys.indexes i "
                    "JOIN sys.index_columns ic ON i.object_id = ic.object_id "
                    "AND i.index_id = ic.index_id "
                    "JOIN sys.columns c ON ic.object_id = c.object_id "
                    "AND ic.column_id = c.column_id "
                    "WHERE i.object_id = OBJECT_ID('{}.{}') "
                    "AND i.is_primary_key = 0 AND i.type > 0 "
                    "ORDER BY i.name, ic.key_ordinal",
                    escapeLiteral(schema), escapeLiteral(tableName)),
        10000);
    if (idxs.success() && !idxs.empty()) {
        for (const auto& row : idxs[0].tableData) {
            if (row.size() < 3)
                continue;
            if (t.indexes.empty() || t.indexes.back().name != row[0]) {
                Index idx;
                idx.name = row[0];
                idx.isUnique = (row[1] == "1");
                idx.columns.push_back(row[2]);
                t.indexes.push_back(std::move(idx));
            } else {
                t.indexes.back().columns.push_back(row[2]);
            }
        }
    }

    buildForeignKeyLookup(t);
    return t;
}

// ---------- MSSQLDatabase member impls ----------

QueryResult MSSQLDatabase::execute(const std::string& sql, int rowLimit) {
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
    int limit = rowLimit > 0 ? rowLimit : INT_MAX;
    result = executeQueryOnProcess(conn_, sql, limit);
    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return result;
}

std::vector<DatabasePtr> MSSQLDatabase::schemas() {
    std::vector<DatabasePtr> out;
    auto names = loadSchemaNames(*this);
    auto self = shared_from_this();
    for (const auto& n : names) {
        out.push_back(std::make_shared<MSSQLSchema>(self, n));
    }
    return out;
}

std::vector<Table> MSSQLDatabase::tables() {
    std::vector<Table> out;
    auto schemaNames = loadSchemaNames(*this);
    for (const auto& s : schemaNames) {
        auto ts = loadTablesForSchema(*this, s);
        out.insert(out.end(), ts.begin(), ts.end());
    }
    return out;
}

std::vector<Table> MSSQLDatabase::views() {
    std::vector<Table> out;
    auto schemaNames = loadSchemaNames(*this);
    for (const auto& s : schemaNames) {
        auto vs = loadViewsForSchema(*this, s);
        out.insert(out.end(), vs.begin(), vs.end());
    }
    return out;
}

std::vector<Routine> MSSQLDatabase::routines() {
    std::vector<Routine> out;
    auto schemaNames = loadSchemaNames(*this);
    for (const auto& s : schemaNames) {
        auto rs = loadRoutinesForSchema(*this, s);
        out.insert(out.end(), rs.begin(), rs.end());
    }
    return out;
}

Table MSSQLDatabase::describeTable(const std::string& tableName) {
    // accept "schema.table" or bare table (default dbo).
    auto dotPos = tableName.find('.');
    std::string schema = "dbo";
    std::string tn = tableName;
    if (dotPos != std::string::npos) {
        schema = tableName.substr(0, dotPos);
        tn = tableName.substr(dotPos + 1);
    }
    return describeTableInSchema(*this, schema, tn);
}

std::vector<std::vector<std::string>>
MSSQLDatabase::getTableData(const Table& table, int limit, int offset,
                            const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> data;
    std::string sql = "SELECT * FROM " + fullyQualified(table);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    // MSSQL OFFSET/FETCH NEXT requires ORDER BY; synthesize one if missing.
    sql += " ORDER BY " + (orderByClause.empty() ? std::string("(SELECT NULL)") : orderByClause);
    sql += std::format(" OFFSET {} ROWS FETCH NEXT {} ROWS ONLY", offset, limit);
    auto r = execute(sql, limit);
    if (!r.success() || r.empty())
        return data;
    return r[0].tableData;
}

std::vector<std::string> MSSQLDatabase::getColumnNames(const Table& table) {
    std::vector<std::string> names;
    const std::string schema = table.schema.empty() ? "dbo" : table.schema;
    auto r = execute(std::format("SELECT COLUMN_NAME FROM INFORMATION_SCHEMA.COLUMNS "
                                 "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{}' "
                                 "AND TABLE_NAME = '{}' ORDER BY ORDINAL_POSITION",
                                 escapeLiteral(schema), escapeLiteral(table.name)),
                     10000);
    if (!r.success() || r.empty())
        return names;
    for (const auto& row : r[0].tableData)
        if (!row.empty())
            names.push_back(row[0]);
    return names;
}

int MSSQLDatabase::getRowCount(const Table& table, const std::string& whereClause) {
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

Status MSSQLDatabase::createTable(const Table& table) {
    const std::string schema = table.schema.empty() ? "dbo" : table.schema;
    std::string sql =
        "CREATE TABLE " + quoteIdent(schema) + "." + quoteIdent(table.name) + " (";
    bool first = true;
    for (const auto& c : table.columns) {
        if (!first)
            sql += ", ";
        first = false;
        sql += quoteIdent(c.name) + " " + (c.type.empty() ? std::string("NVARCHAR(MAX)") : c.type);
        if (c.isAutoIncrement)
            sql += " IDENTITY(1,1)";
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

Status MSSQLDatabase::renameTable(const std::string& oldName, const std::string& newName) {
    auto r = execute(std::format("EXEC sp_rename 'dbo.{}', '{}'", oldName, newName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status MSSQLDatabase::dropTable(const std::string& tableName) {
    auto r = execute(std::format("DROP TABLE [dbo].{}", quoteIdent(tableName)), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status MSSQLDatabase::truncateTable(const std::string& tableName) {
    auto r = execute(std::format("TRUNCATE TABLE [dbo].{}", quoteIdent(tableName)), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status MSSQLDatabase::dropColumn(const std::string& tableName, const std::string& columnName) {
    auto r = execute(std::format("ALTER TABLE [dbo].{} DROP COLUMN {}", quoteIdent(tableName),
                                 quoteIdent(columnName)),
                     0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}
Status MSSQLDatabase::dropView(const std::string& viewName, bool /*isMaterialized*/) {
    auto r = execute(std::format("DROP VIEW [dbo].{}", quoteIdent(viewName)), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

// ---------- MSSQLConnection impl ----------

class ConnectionImpl {
public:
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {
        if (info_.database.empty())
            info_.database = "master";
        initDbLib();
    }

    Status open() {
        try {
            auto db = std::dynamic_pointer_cast<MSSQLDatabase>(database(info_.database));
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
        auto def = std::dynamic_pointer_cast<MSSQLDatabase>(database(info_.database));
        if (!def)
            return {};
        auto r = def->execute("SELECT name FROM sys.databases ORDER BY name", 10000);
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
        auto db = std::make_shared<MSSQLDatabase>(info_, n);
        cache_[n] = db;
        return db;
    }

    Status createDatabase(const CreateDatabaseOptions& opts) {
        if (opts.name.empty())
            return {false, "Database name cannot be empty"};
        try {
            // open a temporary connection to master to issue the DDL.
            DbProcessGuard tmp(openDbLibConnection(info_, "master"));
            clearLastError();
            std::string sql = std::format("CREATE DATABASE {}", quoteIdent(opts.name));
            dbcmd(tmp.get(), sql.c_str());
            if (dbsqlexec(tmp.get()) == FAIL) {
                return {false, getLastError()};
            }
            drainResults(tmp.get());
            return {true, ""};
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    }

    Status dropDatabase(const std::string& name) {
        if (name.empty())
            return {false, "Database name cannot be empty"};
        try {
            // remove the cached handle so its DBPROCESS releases any session on it.
            cache_.erase(name);

            DbProcessGuard tmp(openDbLibConnection(info_, "master"));
            clearLastError();
            std::string sql = std::format(
                "ALTER DATABASE {0} SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE {0}",
                quoteIdent(name));
            dbcmd(tmp.get(), sql.c_str());
            if (dbsqlexec(tmp.get()) == FAIL) {
                std::string err = getLastError();
                drainResults(tmp.get());
                return {false, err};
            }
            drainResults(tmp.get());
            return {true, ""};
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    }

    Status renameDatabase(const std::string& oldName, const std::string& newName) {
        try {
            cache_.erase(oldName);
            DbProcessGuard tmp(openDbLibConnection(info_, "master"));
            clearLastError();
            std::string sql = std::format("ALTER DATABASE {} MODIFY NAME = {}",
                                          quoteIdent(oldName), quoteIdent(newName));
            dbcmd(tmp.get(), sql.c_str());
            if (dbsqlexec(tmp.get()) == FAIL) {
                std::string err = getLastError();
                drainResults(tmp.get());
                return {false, err};
            }
            drainResults(tmp.get());
            return {true, ""};
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    }

private:
    ConnectionInfo info_;
    bool open_ = false;
    std::unordered_map<std::string, std::shared_ptr<MSSQLDatabase>> cache_;
};

} // namespace

MSSQLConnection::MSSQLConnection(const ConnectionInfo& info) : info_(info) {
    impl_ = new ConnectionImpl(info);
}
MSSQLConnection::~MSSQLConnection() {
    delete static_cast<ConnectionImpl*>(impl_);
}
Status MSSQLConnection::open() {
    return static_cast<ConnectionImpl*>(impl_)->open();
}
void MSSQLConnection::close() {
    static_cast<ConnectionImpl*>(impl_)->close();
}
bool MSSQLConnection::isOpen() const {
    return static_cast<const ConnectionImpl*>(impl_)->isOpen();
}
std::vector<DatabasePtr> MSSQLConnection::databases() {
    return static_cast<ConnectionImpl*>(impl_)->databases();
}
DatabasePtr MSSQLConnection::database(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->database(name);
}
Status MSSQLConnection::createDatabase(const CreateDatabaseOptions& opts) {
    return static_cast<ConnectionImpl*>(impl_)->createDatabase(opts);
}
Status MSSQLConnection::dropDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->dropDatabase(name);
}
Status MSSQLConnection::renameDatabase(const std::string& oldName, const std::string& newName) {
    return static_cast<ConnectionImpl*>(impl_)->renameDatabase(oldName, newName);
}

} // namespace dearsql
