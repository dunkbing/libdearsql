#include "dearsql/backends/mssql_connection.hpp"

#include <atomic>
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

// db-lib handlers are process-global; they run on the thread that drives the
// DBPROCESS, so per-thread buffers route errors and messages to the right call
thread_local std::string tls_lastError;
// PRINT output and RAISERROR <= 10
thread_local std::vector<std::string> tls_infoMessages;

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

int dblibMessageHandler(DBPROCESS* /*dbproc*/, DBINT msgno, int /*msgstate*/, int severity,
                        char* msgtext, char* /*srvname*/, char* /*procname*/, int /*line*/) {
    if (!msgtext)
        return 0;
    if (severity > 10) {
        tls_lastError = msgtext;
        return 0;
    }
    // skip session noise (database context, language, charset changed)
    if (msgtext[0] && msgno != 5701 && msgno != 5703 && msgno != 5704)
        tls_infoMessages.emplace_back(msgtext);
    return 0;
}

// interrupt hooks: db-lib polls them while waiting on the server
int checkInterrupt(void* dbproc) {
    auto* flag = reinterpret_cast<std::atomic<bool>*>(dbgetuserdata(static_cast<DBPROCESS*>(dbproc)));
    return flag && flag->load() ? 1 : 0;
}

int handleInterrupt(void* /*dbproc*/) {
    return INT_CANCEL;
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

// '[schema].[name]' as a string literal, for OBJECT_ID()
std::string objectLiteral(const std::string& schema, const std::string& name) {
    return "'" + escapeLiteral(quoteIdent(schema) + "." + quoteIdent(name)) + "'";
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

void drainResults(DBPROCESS* dbproc) {
    while (dbresults(dbproc) != NO_MORE_RESULTS) {
        while (dbnextrow(dbproc) != NO_MORE_ROWS) {
        }
    }
}

StatementResult failed(std::string message) {
    StatementResult r;
    r.success = false;
    r.errorMessage = std::move(message);
    return r;
}

StatementResult extractDbLibResult(DBPROCESS* dbproc, int rowLimit,
                                   const std::atomic<bool>& cancelled) {
    StatementResult result;
    int numCols = dbnumcols(dbproc);

    if (numCols > 0) {
        for (int i = 1; i <= numCols; i++) {
            result.columnNames.emplace_back(dbcolname(dbproc, i));
        }
        bool limited = false;
        STATUS rowCode;
        // rows past the limit are drained so dblib state stays clean
        while (!cancelled && (rowCode = dbnextrow(dbproc)) != NO_MORE_ROWS && rowCode != FAIL) {
            if (static_cast<int>(result.tableData.size()) >= rowLimit) {
                limited = true;
                continue;
            }
            std::vector<std::string> rowData;
            rowData.reserve(numCols);
            for (int i = 1; i <= numCols; i++) {
                rowData.push_back(colToString(dbproc, i));
            }
            result.tableData.push_back(std::move(rowData));
        }
        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
        if (limited)
            result.message += std::format(" (limited to {})", rowLimit);
    } else {
        DBINT affected = DBCOUNT(dbproc);
        if (affected >= 0) {
            result.message = std::format("{} row(s) affected", affected);
            result.affectedRows = static_cast<int>(affected);
        } else {
            result.message = "Query executed successfully";
        }
    }
    return result;
}

QueryResult executeQueryOnProcess(DBPROCESS* dbproc, const std::string& query, int rowLimit,
                                  const std::atomic<bool>& cancelled) {
    QueryResult result;
    clearLastError();
    tls_infoMessages.clear();
    dbcmd(dbproc, query.c_str());

    auto errorText = [&] { return cancelled ? std::string("Query cancelled") : getLastError(); };

    if (dbsqlexec(dbproc) == FAIL) {
        result.statements.push_back(failed(errorText()));
        if (!dbdead(dbproc))
            dbcancel(dbproc);
    } else {
        RETCODE rc;
        while ((rc = dbresults(dbproc)) != NO_MORE_RESULTS) {
            if (rc == FAIL) {
                result.statements.push_back(failed(errorText()));
                break;
            }
            result.statements.push_back(extractDbLibResult(dbproc, rowLimit, cancelled));
            if (cancelled) {
                result.statements.back() = failed("Query cancelled");
                if (!dbdead(dbproc))
                    dbcancel(dbproc);
                break;
            }
        }
    }
    // PRINT output arrives through the message handler during exec/results/rows
    result.messages = std::move(tls_infoMessages);
    tls_infoMessages.clear();
    return result;
}

DBPROCESS* openDbLibConnection(const ConnectionInfo& info, const std::string& dbName) {
    LOGINREC* login = dblogin();
    if (!login)
        throw Error("dblogin() failed");

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
        throw Error("MSSQL connection failed: " + getLastError());
    }

    const std::string targetDb = !dbName.empty() ? dbName : info.database;
    if (!targetDb.empty()) {
        clearLastError();
        if (dbuse(dbproc, targetDb.c_str()) != SUCCEED) {
            std::string err = getLastError();
            dbclose(dbproc);
            throw Error("MSSQL dbuse failed: " + err);
        }
    }
    tls_infoMessages.clear(); // login chatter
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

Status execOnMaster(const ConnectionInfo& info, const std::string& sql) {
    try {
        DbProcessGuard tmp(openDbLibConnection(info, "master"));
        clearLastError();
        dbcmd(tmp.get(), sql.c_str());
        if (dbsqlexec(tmp.get()) == FAIL) {
            std::string err = getLastError();
            return {false, err};
        }
        drainResults(tmp.get());
        tls_infoMessages.clear();
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

// first result set of a catalog query; throws on failure
std::vector<std::vector<std::string>> rows(IDatabase& db, const std::string& sql) {
    auto r = db.execute(sql, 0);
    if (!r.success())
        throw Error(r.errorMessage().empty() ? "query failed" : r.errorMessage());
    return std::move(r[0].tableData);
}

std::string inList(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i)
            out += ", ";
        out += "'" + escapeLiteral(names[i]) + "'";
    }
    return out;
}

std::vector<std::string> loadSchemaNames(IDatabase& db) {
    std::vector<std::string> out;
    for (const auto& row : rows(db, "SELECT SCHEMA_NAME FROM INFORMATION_SCHEMA.SCHEMATA "
                                    "WHERE CATALOG_NAME = DB_NAME() "
                                    "AND SCHEMA_NAME NOT IN ('INFORMATION_SCHEMA', 'sys', "
                                    "'db_owner', 'db_accessadmin', 'db_securityadmin', "
                                    "'db_ddladmin', 'db_backupoperator', 'db_datareader', "
                                    "'db_datawriter', 'db_denydatareader', 'db_denydatawriter') "
                                    "ORDER BY SCHEMA_NAME")) {
        if (!row.empty() && !isNullSentinel(row[0]))
            out.push_back(row[0]);
    }
    return out;
}

std::vector<Table> loadTablesForSchema(IDatabase& db, const std::string& schema) {
    std::vector<Table> result;
    const std::string s = escapeLiteral(schema);

    std::vector<std::string> tableNames;
    for (const auto& row : rows(db, std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.TABLES "
                                                "WHERE TABLE_TYPE = 'BASE TABLE' "
                                                "AND TABLE_CATALOG = DB_NAME() "
                                                "AND TABLE_SCHEMA = '{}' ORDER BY TABLE_NAME",
                                                s))) {
        if (!row.empty())
            tableNames.push_back(row[0]);
    }
    if (tableNames.empty())
        return result;
    const std::string in = inList(tableNames);

    std::map<std::string, std::vector<Column>> tableColumns;
    for (const auto& row :
         rows(db, std::format("SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE, "
                              "COLUMNPROPERTY(OBJECT_ID(QUOTENAME(TABLE_SCHEMA) + '.' + "
                              "QUOTENAME(TABLE_NAME)), COLUMN_NAME, 'IsIdentity'), "
                              "ISNULL(COLUMN_DEFAULT, '') "
                              "FROM INFORMATION_SCHEMA.COLUMNS "
                              "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                              "AND TABLE_NAME IN ({1}) ORDER BY TABLE_NAME, ORDINAL_POSITION",
                              s, in))) {
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

    for (const auto& row :
         rows(db, std::format("SELECT tc.TABLE_NAME, c.COLUMN_NAME "
                              "FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
                              "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE c "
                              "ON c.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                              "AND c.TABLE_SCHEMA = tc.TABLE_SCHEMA "
                              "WHERE tc.TABLE_SCHEMA = '{0}' AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY' "
                              "AND tc.TABLE_NAME IN ({1})",
                              s, in))) {
        if (row.size() < 2)
            continue;
        auto it = tableColumns.find(row[0]);
        if (it == tableColumns.end())
            continue;
        for (auto& c : it->second)
            if (c.name == row[1])
                c.isPrimaryKey = true;
    }

    std::map<std::string, std::vector<ForeignKey>> tableFks;
    for (const auto& row :
         rows(db, std::format("SELECT OBJECT_NAME(fk.parent_object_id), fk.name, "
                              "COL_NAME(fkc.parent_object_id, fkc.parent_column_id), "
                              "OBJECT_NAME(fkc.referenced_object_id), "
                              "COL_NAME(fkc.referenced_object_id, fkc.referenced_column_id) "
                              "FROM sys.foreign_keys fk "
                              "JOIN sys.foreign_key_columns fkc "
                              "ON fk.object_id = fkc.constraint_object_id "
                              "WHERE OBJECT_SCHEMA_NAME(fk.parent_object_id) = '{}'",
                              s))) {
        if (row.size() < 5)
            continue;
        ForeignKey fk;
        fk.name = row[1];
        fk.sourceColumn = row[2];
        fk.targetTable = row[3];
        fk.targetColumn = row[4];
        tableFks[row[0]].push_back(std::move(fk));
    }

    std::map<std::string, std::vector<Index>> tableIndexes;
    for (const auto& row :
         rows(db, std::format("SELECT OBJECT_NAME(i.object_id), i.name, i.is_unique, c.name "
                              "FROM sys.indexes i "
                              "JOIN sys.index_columns ic ON i.object_id = ic.object_id "
                              "AND i.index_id = ic.index_id "
                              "JOIN sys.columns c ON ic.object_id = c.object_id "
                              "AND ic.column_id = c.column_id "
                              "WHERE i.is_primary_key = 0 AND i.type > 0 "
                              "AND OBJECT_SCHEMA_NAME(i.object_id) = '{}' "
                              "ORDER BY OBJECT_NAME(i.object_id), i.name, ic.key_ordinal",
                              s))) {
        if (row.size() < 4)
            continue;
        auto& vec = tableIndexes[row[0]];
        if (vec.empty() || vec.back().name != row[1]) {
            Index idx;
            idx.name = row[1];
            idx.isUnique = (row[2] == "1");
            vec.push_back(std::move(idx));
        }
        vec.back().columns.push_back(row[3]);
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

std::vector<Table> loadViewsForSchema(IDatabase& db, const std::string& schema) {
    std::vector<Table> result;
    const std::string s = escapeLiteral(schema);

    std::vector<std::string> viewNames;
    for (const auto& row : rows(db, std::format("SELECT TABLE_NAME FROM INFORMATION_SCHEMA.VIEWS "
                                                "WHERE TABLE_CATALOG = DB_NAME() "
                                                "AND TABLE_SCHEMA = '{}' ORDER BY TABLE_NAME",
                                                s))) {
        if (!row.empty())
            viewNames.push_back(row[0]);
    }
    if (viewNames.empty())
        return result;

    std::map<std::string, std::vector<Column>> viewColumns;
    for (const auto& row :
         rows(db, std::format("SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, IS_NULLABLE "
                              "FROM INFORMATION_SCHEMA.COLUMNS "
                              "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                              "AND TABLE_NAME IN ({1}) ORDER BY TABLE_NAME, ORDINAL_POSITION",
                              s, inList(viewNames)))) {
        if (row.size() < 4)
            continue;
        Column col;
        col.name = row[1];
        col.type = row[2];
        col.isNotNull = row[3] == "NO";
        viewColumns[row[0]].push_back(std::move(col));
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

std::vector<Routine> loadRoutinesForSchema(IDatabase& db, const std::string& schema) {
    std::vector<Routine> out;
    // parameter_id 0 is a scalar function's return value
    for (const auto& row : rows(
             db,
             std::format(
                 "SELECT o.name, "
                 "o.name + '(' + ISNULL(STUFF((SELECT ', ' + p.name + ' ' + "
                 "TYPE_NAME(p.user_type_id) FROM sys.parameters p "
                 "WHERE p.object_id = o.object_id AND p.parameter_id > 0 "
                 "ORDER BY p.parameter_id FOR XML PATH('')), 1, 2, ''), '') + ')', "
                 "CASE o.type WHEN 'P' THEN 'PROCEDURE' ELSE 'FUNCTION' END, "
                 "CASE WHEN o.type IN ('IF', 'TF') THEN 'TABLE' ELSE ISNULL((SELECT "
                 "TYPE_NAME(p.user_type_id) FROM sys.parameters p WHERE p.object_id = "
                 "o.object_id AND p.parameter_id = 0), '') END "
                 "FROM sys.objects o "
                 "WHERE o.schema_id = SCHEMA_ID('{}') "
                 "AND o.type IN ('P', 'FN', 'IF', 'TF') "
                 "ORDER BY o.type, o.name",
                 escapeLiteral(schema)))) {
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

Table describeTableInSchema(IDatabase& db, const std::string& schema,
                            const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.schema = schema;
    t.fullName = schema + "." + tableName;
    const std::string s = escapeLiteral(schema);
    const std::string n = escapeLiteral(tableName);
    const std::string obj = objectLiteral(schema, tableName);

    for (const auto& row :
         rows(db, std::format("SELECT COLUMN_NAME, DATA_TYPE + CASE "
                              "WHEN DATA_TYPE IN ('varchar', 'char', 'nvarchar', 'nchar', "
                              "'varbinary', 'binary') THEN '(' + CASE WHEN "
                              "CHARACTER_MAXIMUM_LENGTH = -1 THEN 'max' ELSE "
                              "CAST(CHARACTER_MAXIMUM_LENGTH AS varchar(10)) END + ')' "
                              "WHEN DATA_TYPE IN ('decimal', 'numeric') THEN '(' + "
                              "CAST(NUMERIC_PRECISION AS varchar(5)) + ',' + "
                              "CAST(NUMERIC_SCALE AS varchar(5)) + ')' "
                              "WHEN DATA_TYPE IN ('datetime2', 'time', 'datetimeoffset') "
                              "THEN '(' + CAST(DATETIME_PRECISION AS varchar(5)) + ')' "
                              "ELSE '' END, IS_NULLABLE, "
                              "ISNULL(COLUMN_DEFAULT, ''), "
                              "COLUMNPROPERTY(OBJECT_ID({2}), COLUMN_NAME, 'IsIdentity') "
                              "FROM INFORMATION_SCHEMA.COLUMNS "
                              "WHERE TABLE_CATALOG = DB_NAME() AND TABLE_SCHEMA = '{0}' "
                              "AND TABLE_NAME = '{1}' ORDER BY ORDINAL_POSITION",
                              s, n, obj))) {
        if (row.size() < 5)
            continue;
        Column c;
        c.name = row[0];
        c.type = row[1];
        c.isNotNull = row[2] == "NO";
        // db-lib pads the ISNULL() fallback
        c.defaultValue = row[3].substr(0, row[3].find_last_not_of(' ') + 1);
        c.isAutoIncrement = row[4] == "1";
        t.columns.push_back(std::move(c));
    }

    for (const auto& row :
         rows(db, std::format("SELECT c.COLUMN_NAME FROM INFORMATION_SCHEMA.TABLE_CONSTRAINTS tc "
                              "JOIN INFORMATION_SCHEMA.KEY_COLUMN_USAGE c "
                              "ON c.CONSTRAINT_NAME = tc.CONSTRAINT_NAME "
                              "AND c.TABLE_SCHEMA = tc.TABLE_SCHEMA "
                              "WHERE tc.TABLE_SCHEMA = '{}' AND tc.TABLE_NAME = '{}' "
                              "AND tc.CONSTRAINT_TYPE = 'PRIMARY KEY'",
                              s, n))) {
        if (row.empty())
            continue;
        for (auto& c : t.columns)
            if (c.name == row[0])
                c.isPrimaryKey = true;
    }

    for (const auto& row :
         rows(db, std::format("SELECT fk.name, "
                              "COL_NAME(fkc.parent_object_id, fkc.parent_column_id), "
                              "OBJECT_NAME(fkc.referenced_object_id), "
                              "COL_NAME(fkc.referenced_object_id, fkc.referenced_column_id) "
                              "FROM sys.foreign_keys fk "
                              "JOIN sys.foreign_key_columns fkc "
                              "ON fk.object_id = fkc.constraint_object_id "
                              "WHERE fk.parent_object_id = OBJECT_ID({})",
                              obj))) {
        if (row.size() < 4)
            continue;
        ForeignKey fk;
        fk.name = row[0];
        fk.sourceColumn = row[1];
        fk.targetTable = row[2];
        fk.targetColumn = row[3];
        t.foreignKeys.push_back(std::move(fk));
    }

    for (const auto& row : rows(db, std::format("SELECT i.name, i.is_unique, c.name "
                                                "FROM sys.indexes i "
                                                "JOIN sys.index_columns ic "
                                                "ON i.object_id = ic.object_id "
                                                "AND i.index_id = ic.index_id "
                                                "JOIN sys.columns c "
                                                "ON ic.object_id = c.object_id "
                                                "AND ic.column_id = c.column_id "
                                                "WHERE i.object_id = OBJECT_ID({}) "
                                                "AND i.is_primary_key = 0 AND i.type > 0 "
                                                "ORDER BY i.name, ic.key_ordinal",
                                                obj))) {
        if (row.size() < 3)
            continue;
        if (t.indexes.empty() || t.indexes.back().name != row[0]) {
            Index idx;
            idx.name = row[0];
            idx.isUnique = (row[1] == "1");
            t.indexes.push_back(std::move(idx));
        }
        t.indexes.back().columns.push_back(row[2]);
    }

    buildForeignKeyLookup(t);
    return t;
}

class MSSQLSchema;

// One database (catalog) over its own DBPROCESS. database() caches one per
// name; openDatabase() hands out fresh ones for host-side pools.
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

    // connect now instead of on first use; throws
    void open() {
        std::lock_guard lock(mu_);
        ensureConn();
    }

    bool alive() override {
        std::lock_guard lock(mu_);
        return !conn_ || !dbdead(conn_);
    }

    // db-lib cannot be driven from two threads: flag it and let the running
    // call notice between rows, or the interrupt hook while it waits on the
    // server (which drops the connection; the next call reconnects)
    void cancel() override {
        // ponytail: a cancel racing the end of a call can still mark the next one;
        // a per-call generation would close that window
        if (inCall_ > 0)
            cancelled_ = true;
    }

    std::vector<DatabasePtr> schemas() override;
    DatabasePtr schema(const std::string& schemaName) override;
    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<Routine> routines() override;
    Table describeTable(const std::string& tableName) override;
    QueryResult execute(const std::string& sql, int rowLimit) override;

private:
    void ensureConn() {
        if (conn_ && !dbdead(conn_))
            return;
        if (conn_) {
            dbclose(conn_);
            conn_ = nullptr;
        }
        initDbLib();
        conn_ = openDbLibConnection(info_, name_);
        dbsetuserdata(conn_, reinterpret_cast<BYTE*>(&cancelled_));
        dbsetinterrupt(conn_, checkInterrupt, handleInterrupt);
    }

    template <typename F> auto forEachSchema(F&& load) {
        decltype(load(std::string())) out;
        for (const auto& s : loadSchemaNames(*this)) {
            auto part = load(s);
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }

    ConnectionInfo info_;
    std::string name_;
    DBPROCESS* conn_ = nullptr;
    std::atomic<bool> cancelled_ = false;
    std::atomic<int> inCall_ = 0; // execute() calls waiting for or holding mu_
    std::mutex mu_;
};

// One schema of a MSSQLDatabase, running on the parent's DBPROCESS. Data
// access and DDL use the IDatabase builder defaults qualified with name().
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
    [[nodiscard]] std::string schemaName() const override {
        return name_;
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
        return parent_->execute(sql, rowLimit);
    }
    bool alive() override {
        return parent_->alive();
    }
    void cancel() override {
        parent_->cancel();
    }

    Status renameSchema(const std::string& /*newName*/) override {
        // no rename DDL; objects would have to be transferred one by one
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

QueryResult MSSQLDatabase::execute(const std::string& sql, int rowLimit) {
    const auto startTime = std::chrono::steady_clock::now();
    ++inCall_;
    std::lock_guard lock(mu_);
    QueryResult result;
    try {
        ensureConn();
        result = executeQueryOnProcess(conn_, sql, rowLimit > 0 ? rowLimit : INT_MAX, cancelled_);
    } catch (const std::exception& e) {
        result.statements.push_back(failed(e.what()));
    }
    // cleared after the run, not before: a cancel that lands while this call
    // waits for mu_ is meant for it
    cancelled_ = false;
    --inCall_;
    result.executionTimeMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime)
            .count();
    return result;
}

std::vector<DatabasePtr> MSSQLDatabase::schemas() {
    std::vector<DatabasePtr> out;
    for (const auto& n : loadSchemaNames(*this))
        out.push_back(schema(n));
    return out;
}

DatabasePtr MSSQLDatabase::schema(const std::string& schemaName) {
    return std::make_shared<MSSQLSchema>(shared_from_this(), schemaName);
}

std::vector<Table> MSSQLDatabase::tables() {
    return forEachSchema([&](const std::string& s) { return loadTablesForSchema(*this, s); });
}

std::vector<Table> MSSQLDatabase::views() {
    return forEachSchema([&](const std::string& s) { return loadViewsForSchema(*this, s); });
}

std::vector<Routine> MSSQLDatabase::routines() {
    return forEachSchema([&](const std::string& s) { return loadRoutinesForSchema(*this, s); });
}

Table MSSQLDatabase::describeTable(const std::string& tableName) {
    // "schema.table", or a bare name in the login's default schema
    if (auto dot = tableName.find('.'); dot != std::string::npos)
        return describeTableInSchema(*this, tableName.substr(0, dot), tableName.substr(dot + 1));
    auto def = rows(*this, "SELECT SCHEMA_NAME()");
    const std::string schema = def.empty() || def[0].empty() ? "dbo" : def[0][0];
    return describeTableInSchema(*this, schema, tableName);
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
            std::static_pointer_cast<MSSQLDatabase>(database(""))->open();
            open_ = true;
            return {true, ""};
        } catch (const std::exception& e) {
            open_ = false;
            return {false, e.what()};
        }
    }

    void close() {
        std::lock_guard lock(mu_);
        cache_.clear();
        open_ = false;
    }
    bool isOpen() const {
        return open_;
    }

    // user databases; the four system ones (master, tempdb, model, msdb) are hidden
    std::vector<DatabasePtr> databases() {
        std::vector<DatabasePtr> out;
        for (const auto& row :
             rows(*database(""), "SELECT name FROM sys.databases WHERE database_id > 4 "
                                 "ORDER BY name")) {
            if (!row.empty())
                out.push_back(database(row[0]));
        }
        return out;
    }

    DatabasePtr database(const std::string& name) {
        std::lock_guard lock(mu_);
        const std::string n = name.empty() ? info_.database : name;
        auto& db = cache_[n];
        if (!db)
            db = std::make_shared<MSSQLDatabase>(info_, n);
        return db;
    }

    DatabasePtr openDatabase(const std::string& name) {
        std::lock_guard lock(mu_);
        return std::make_shared<MSSQLDatabase>(info_, name.empty() ? info_.database : name);
    }

    Status createDatabase(const CreateDatabaseOptions& opts) {
        if (opts.name.empty())
            return {false, "Database name cannot be empty"};
        return execOnMaster(info_, "CREATE DATABASE " + quoteIdent(opts.name));
    }

    // dropping the connected database moves the default to master
    Status dropDatabase(const std::string& name) {
        if (name.empty())
            return {false, "Database name cannot be empty"};
        forget(name);
        auto status = execOnMaster(
            info_, std::format("ALTER DATABASE {0} SET SINGLE_USER WITH ROLLBACK IMMEDIATE; "
                               "DROP DATABASE {0}",
                               quoteIdent(name)));
        if (status.first) {
            std::lock_guard lock(mu_);
            if (name == info_.database)
                info_.database = "master";
        }
        return status;
    }

    Status renameDatabase(const std::string& oldName, const std::string& newName) {
        forget(oldName);
        return execOnMaster(info_, std::format("ALTER DATABASE {} MODIFY NAME = {}",
                                               quoteIdent(oldName), quoteIdent(newName)));
    }

private:
    // drop the cached handle so its session does not block the DDL
    void forget(const std::string& name) {
        std::lock_guard lock(mu_);
        cache_.erase(name);
    }

    ConnectionInfo info_;
    bool open_ = false;
    std::unordered_map<std::string, std::shared_ptr<MSSQLDatabase>> cache_;
    std::mutex mu_;
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
DatabasePtr MSSQLConnection::openDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->openDatabase(name);
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
