#include "dearsql/backends/oracle_connection.hpp"
#include "dearsql/oracle_installer.hpp"

#if defined(__linux__)
#include <dlfcn.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <dpi.h>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace dearsql {

namespace {

// ---------- ODPI-C global context ----------

std::mutex& ctxMutex() {
    static std::mutex m;
    return m;
}

dpiContext*& ctxHandle() {
    static dpiContext* h = nullptr;
    return h;
}

std::string& ctxInitError() {
    static std::string s;
    return s;
}

// kept alive for ODPI's lifetime so oracleClientLibDir doesn't dangle
std::string& clientLibDirHolder() {
    static std::string s;
    return s;
}

bool& autoInstallAttempted() {
    static bool attempted = false;
    return attempted;
}

dpiContext* getDpiContext() {
    std::lock_guard lock(ctxMutex());
    auto& ctx = ctxHandle();
    if (ctx)
        return ctx;
    dpiContextCreateParams params{};

    if (!oracle::isInstalled()) {
        if (autoInstallAttempted()) {
            if (ctxInitError().empty()) {
                ctxInitError() =
                    "Oracle Instant Client is not installed and auto-install already failed";
            }
            return nullptr;
        }
        autoInstallAttempted() = true;
        auto [ok, err] = oracle::install();
        if (!ok) {
            ctxInitError() =
                "Oracle Instant Client auto-install failed: " +
                (err.empty() ? std::string("unknown error") : err);
            return nullptr;
        }
    }

    if (oracle::isInstalled()) {
        clientLibDirHolder() = oracle::installDir();
        params.oracleClientLibDir = clientLibDirHolder().c_str();
#if defined(__linux__)
        // Always re-run libaio bundling on context init. A prior install() may
        // have placed libclntsh.so but failed to fetch libaio (e.g. transient
        // network blip); calling here lets every connect attempt recover.
        oracle::ensureLibaio(clientLibDirHolder());

        // Oracle Instant Client's libclntsh.so DT_NEEDEDs libnnz.so and
        // libclntshcore.so.<v>, both bundled in the same dir — but libclntsh
        // has no $ORIGIN RUNPATH, so ld.so won't find them when ODPI dlopens
        // libclntsh by absolute path. Preload them ourselves with RTLD_GLOBAL
        // (plus libaio.so.1 whose SONAME mismatch we already fixed above) so
        // libclntsh's deps resolve from the in-memory cache.
        //
        // Order matters: libnnz itself DT_NEEDEDs libclntshcore.so.23.1, so
        // libclntshcore must be in the cache before libnnz is loaded.
        std::string libaioPath = clientLibDirHolder() + "/libaio.so.1";
        (void)dlopen(libaioPath.c_str(), RTLD_NOW | RTLD_GLOBAL);

        // libclntshcore's real file carries the full version
        // (libclntshcore.so.23.1); scan to avoid hardcoding it.
        std::error_code ec;
        for (const auto& entry :
             std::filesystem::directory_iterator(clientLibDirHolder(), ec)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("libclntshcore.so.")) {
                (void)dlopen(entry.path().c_str(), RTLD_LAZY | RTLD_GLOBAL);
            }
        }

        std::string libnnzPath = clientLibDirHolder() + "/libnnz.so";
        (void)dlopen(libnnzPath.c_str(), RTLD_LAZY | RTLD_GLOBAL);
#endif
    }

    dpiErrorInfo errorInfo;
    if (dpiContext_createWithParams(DPI_MAJOR_VERSION, DPI_MINOR_VERSION, &params, &ctx,
                                    &errorInfo) != DPI_SUCCESS) {
        ctxInitError().assign(errorInfo.message, errorInfo.messageLength);
        ctx = nullptr;
        return nullptr;
    }
    return ctx;
}

std::string lastInitError() {
    std::lock_guard lock(ctxMutex());
    return ctxInitError();
}

// ---------- helpers ----------

std::string dpiErrText(dpiContext* ctx) {
    if (!ctx)
        return "Oracle context not initialized";
    dpiErrorInfo info;
    dpiContext_getError(ctx, &info);
    return std::string(info.message, info.messageLength);
}

std::string toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
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
    std::string out = "\"";
    out.reserve(id.size() + 2);
    for (char c : id) {
        if (c == '"')
            out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string dataToString(dpiNativeTypeNum nativeType, dpiData* data) {
    if (data->isNull)
        return std::string(NULL_SENTINEL);
    switch (nativeType) {
    case DPI_NATIVE_TYPE_BYTES:
        return std::string(data->value.asBytes.ptr, data->value.asBytes.length);
    case DPI_NATIVE_TYPE_DOUBLE:
        return std::format("{}", data->value.asDouble);
    case DPI_NATIVE_TYPE_FLOAT:
        return std::format("{}", data->value.asFloat);
    case DPI_NATIVE_TYPE_INT64:
        return std::format("{}", data->value.asInt64);
    case DPI_NATIVE_TYPE_UINT64:
        return std::format("{}", data->value.asUint64);
    case DPI_NATIVE_TYPE_BOOLEAN:
        return data->value.asBoolean ? std::string(BOOL_TRUE_SENTINEL)
                                     : std::string(BOOL_FALSE_SENTINEL);
    case DPI_NATIVE_TYPE_TIMESTAMP: {
        auto& ts = data->value.asTimestamp;
        return std::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}", ts.year, ts.month, ts.day,
                           ts.hour, ts.minute, ts.second);
    }
    case DPI_NATIVE_TYPE_INTERVAL_DS: {
        auto& iv = data->value.asIntervalDS;
        return std::format("{} {:02d}:{:02d}:{:02d}", iv.days, iv.hours, iv.minutes, iv.seconds);
    }
    case DPI_NATIVE_TYPE_INTERVAL_YM: {
        auto& iv = data->value.asIntervalYM;
        return std::format("{}-{}", iv.years, iv.months);
    }
    default:
        return "<unsupported>";
    }
}

struct StmtDeleter {
    void operator()(dpiStmt* s) const {
        if (s)
            dpiStmt_release(s);
    }
};
using StmtPtr = std::unique_ptr<dpiStmt, StmtDeleter>;

// run a query, fill StatementResult. assumes the caller holds the conn mutex.
QueryResult runQuery(dpiContext* ctx, dpiConn* conn, const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::high_resolution_clock::now();

    if (!conn) {
        StatementResult r;
        r.success = false;
        r.errorMessage = "Not connected to Oracle";
        result.statements.push_back(std::move(r));
        return result;
    }

    dpiStmt* raw = nullptr;
    if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()), nullptr, 0,
                            &raw) != DPI_SUCCESS) {
        StatementResult r;
        r.success = false;
        r.errorMessage = dpiErrText(ctx);
        result.statements.push_back(std::move(r));
        return result;
    }
    StmtPtr stmt(raw);

    dpiStmtInfo stmtInfo;
    dpiStmt_getInfo(raw, &stmtInfo);
    dpiExecMode execMode =
        stmtInfo.isQuery ? DPI_MODE_EXEC_DEFAULT : DPI_MODE_EXEC_COMMIT_ON_SUCCESS;

    uint32_t numCols = 0;
    if (dpiStmt_execute(raw, execMode, &numCols) != DPI_SUCCESS) {
        StatementResult r;
        r.success = false;
        r.errorMessage = dpiErrText(ctx);
        result.statements.push_back(std::move(r));
        return result;
    }

    StatementResult r;
    if (numCols > 0) {
        for (uint32_t i = 1; i <= numCols; i++) {
            dpiQueryInfo qi;
            dpiStmt_getQueryInfo(raw, i, &qi);
            r.columnNames.emplace_back(qi.name, qi.nameLength);
        }
        int rowCount = 0;
        int found = 0;
        uint32_t bufIdx = 0;
        while ((rowLimit <= 0 || rowCount < rowLimit) &&
               dpiStmt_fetch(raw, &found, &bufIdx) == DPI_SUCCESS && found) {
            std::vector<std::string> row;
            row.reserve(numCols);
            for (uint32_t i = 1; i <= numCols; i++) {
                dpiNativeTypeNum nt;
                dpiData* d;
                dpiStmt_getQueryValue(raw, i, &nt, &d);
                row.push_back(dataToString(nt, d));
            }
            r.tableData.push_back(std::move(row));
            rowCount++;
        }
        r.message = std::format("Returned {} row{}", r.tableData.size(),
                                r.tableData.size() == 1 ? "" : "s");
        r.success = true;
    } else {
        uint64_t rowsAffected = 0;
        dpiStmt_getRowCount(raw, &rowsAffected);
        r.affectedRows = static_cast<int>(rowsAffected);
        if (rowsAffected > 0)
            r.message = std::format("{} row(s) affected", rowsAffected);
        else
            r.message = "Query executed successfully";
        r.success = true;
    }
    result.statements.push_back(std::move(r));

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    return result;
}

// fetch single-column string list (drops NULLs)
std::vector<std::string> queryStringList(dpiContext* ctx, dpiConn* conn, const std::string& sql) {
    std::vector<std::string> out;
    if (!conn)
        return out;
    dpiStmt* raw = nullptr;
    if (dpiConn_prepareStmt(conn, 0, sql.c_str(), static_cast<uint32_t>(sql.size()), nullptr, 0,
                            &raw) != DPI_SUCCESS)
        return out;
    StmtPtr stmt(raw);
    uint32_t numCols = 0;
    if (dpiStmt_execute(raw, DPI_MODE_EXEC_DEFAULT, &numCols) != DPI_SUCCESS || numCols == 0)
        return out;
    int found = 0;
    uint32_t bufIdx = 0;
    while (dpiStmt_fetch(raw, &found, &bufIdx) == DPI_SUCCESS && found) {
        dpiNativeTypeNum nt;
        dpiData* d;
        dpiStmt_getQueryValue(raw, 1, &nt, &d);
        if (!d->isNull)
            out.push_back(dataToString(nt, d));
    }
    return out;
}

std::string queryScalar(dpiContext* ctx, dpiConn* conn, const std::string& sql) {
    auto list = queryStringList(ctx, conn, sql);
    return list.empty() ? "" : list[0];
}

// ---------- OracleDatabase: one per Oracle user/schema ----------

class OracleDatabase final : public IDatabase {
public:
    OracleDatabase(dpiContext* ctx, dpiConn* conn, std::mutex& connMutex, std::string connName,
                   std::string schemaName, bool ownConn)
        : ctx_(ctx), conn_(conn), connMutex_(&connMutex), connName_(std::move(connName)),
          schema_(std::move(schemaName)), ownConn_(ownConn) {}

    ~OracleDatabase() override {
        if (ownConn_ && conn_) {
            dpiConn_release(conn_);
            conn_ = nullptr;
        }
    }

    [[nodiscard]] std::string name() const override {
        return schema_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::ORACLE;
    }

    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<std::string> sequences() override;
    std::vector<Routine> routines() override;
    Table describeTable(const std::string& tableName) override;

    QueryResult execute(const std::string& sql, int rowLimit) override {
        std::lock_guard lock(*connMutex_);
        return runQuery(ctx_, conn_, sql, rowLimit);
    }

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

private:
    std::string qualifiedTable(const Table& t) const {
        const std::string sch = t.schema.empty() ? schema_ : t.schema;
        return quoteIdent(sch) + "." + quoteIdent(t.name);
    }
    std::string qualifiedTable(const std::string& tableName) const {
        return quoteIdent(schema_) + "." + quoteIdent(tableName);
    }

    QueryResult execLocked(const std::string& sql, int rowLimit = 0) {
        return runQuery(ctx_, conn_, sql, rowLimit);
    }

    dpiContext* ctx_;
    dpiConn* conn_;
    std::mutex* connMutex_;
    std::string connName_;
    std::string schema_;
    bool ownConn_;
};

std::vector<Table> OracleDatabase::tables() {
    std::vector<Table> result;
    std::lock_guard lock(*connMutex_);

    auto tableNames = queryStringList(
        ctx_, conn_,
        std::format("SELECT TABLE_NAME FROM ALL_TABLES WHERE OWNER = '{}' ORDER BY TABLE_NAME",
                    escapeLiteral(schema_)));
    if (tableNames.empty())
        return result;

    // batch-load columns
    std::unordered_map<std::string, std::vector<Column>> allColumns;
    {
        std::string q = std::format(
            "SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, NULLABLE, DATA_LENGTH, DATA_PRECISION, "
            "DATA_SCALE, IDENTITY_COLUMN "
            "FROM ALL_TAB_COLUMNS WHERE OWNER = '{}' ORDER BY TABLE_NAME, COLUMN_ID",
            escapeLiteral(schema_));
        auto r = runQuery(ctx_, conn_, q, 1000000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 8)
                    continue;
                Column c;
                c.name = row[1];
                const std::string& dataType = row[2];
                c.isNotNull = (row[3] == "N");
                const std::string& dataLen = row[4];
                const std::string& prec = row[5];
                const std::string& scale = row[6];
                c.isAutoIncrement = (row[7] == "YES");

                if (!isNullSentinel(prec)) {
                    if (!isNullSentinel(scale) && scale != "0")
                        c.type = std::format("{}({},{})", dataType, prec, scale);
                    else
                        c.type = std::format("{}({})", dataType, prec);
                } else if (dataType == "VARCHAR2" || dataType == "CHAR" ||
                           dataType == "NVARCHAR2" || dataType == "RAW") {
                    c.type = std::format("{}({})", dataType, dataLen);
                } else {
                    c.type = dataType;
                }
                allColumns[row[0]].push_back(std::move(c));
            }
        }
    }

    // batch-load primary keys
    std::unordered_map<std::string, std::vector<std::string>> allPKs;
    {
        std::string q =
            std::format("SELECT ac.TABLE_NAME, acc.COLUMN_NAME FROM ALL_CONS_COLUMNS acc "
                        "JOIN ALL_CONSTRAINTS ac ON acc.CONSTRAINT_NAME = ac.CONSTRAINT_NAME "
                        "AND acc.OWNER = ac.OWNER "
                        "WHERE ac.CONSTRAINT_TYPE = 'P' AND ac.OWNER = '{}'",
                        escapeLiteral(schema_));
        auto r = runQuery(ctx_, conn_, q, 1000000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 2)
                    continue;
                allPKs[row[0]].push_back(row[1]);
            }
        }
    }

    // batch-load foreign keys
    std::unordered_map<std::string, std::vector<ForeignKey>> allFKs;
    {
        std::string q =
            std::format("SELECT ac.TABLE_NAME, ac.CONSTRAINT_NAME, acc.COLUMN_NAME, "
                        "rc.TABLE_NAME, rcc.COLUMN_NAME "
                        "FROM ALL_CONSTRAINTS ac "
                        "JOIN ALL_CONS_COLUMNS acc ON ac.CONSTRAINT_NAME = acc.CONSTRAINT_NAME "
                        "AND ac.OWNER = acc.OWNER "
                        "JOIN ALL_CONSTRAINTS rc ON ac.R_CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
                        "AND ac.R_OWNER = rc.OWNER "
                        "JOIN ALL_CONS_COLUMNS rcc ON rc.CONSTRAINT_NAME = rcc.CONSTRAINT_NAME "
                        "AND rc.OWNER = rcc.OWNER AND acc.POSITION = rcc.POSITION "
                        "WHERE ac.CONSTRAINT_TYPE = 'R' AND ac.OWNER = '{}' "
                        "ORDER BY ac.TABLE_NAME, ac.CONSTRAINT_NAME, acc.POSITION",
                        escapeLiteral(schema_));
        auto r = runQuery(ctx_, conn_, q, 1000000);
        if (r.success() && !r.empty()) {
            for (const auto& row : r[0].tableData) {
                if (row.size() < 5)
                    continue;
                ForeignKey fk;
                fk.name = row[1];
                fk.sourceColumn = row[2];
                fk.targetTable = row[3];
                fk.targetColumn = row[4];
                allFKs[row[0]].push_back(std::move(fk));
            }
        }
    }

    // batch-load indexes
    std::unordered_map<std::string, std::vector<Index>> allIndexes;
    {
        std::string q =
            std::format("SELECT i.TABLE_NAME, i.INDEX_NAME, i.UNIQUENESS, ic.COLUMN_NAME "
                        "FROM ALL_INDEXES i "
                        "JOIN ALL_IND_COLUMNS ic ON i.INDEX_NAME = ic.INDEX_NAME "
                        "AND i.OWNER = ic.INDEX_OWNER "
                        "LEFT JOIN ALL_CONSTRAINTS c ON i.INDEX_NAME = c.INDEX_NAME "
                        "AND i.OWNER = c.OWNER "
                        "WHERE i.OWNER = '{}' "
                        "AND (c.CONSTRAINT_TYPE IS NULL OR c.CONSTRAINT_TYPE != 'P') "
                        "ORDER BY i.TABLE_NAME, i.INDEX_NAME, ic.COLUMN_POSITION",
                        escapeLiteral(schema_));
        auto r = runQuery(ctx_, conn_, q, 1000000);
        if (r.success() && !r.empty()) {
            std::unordered_map<std::string, std::map<std::string, Index>> tableIndexMap;
            for (const auto& row : r[0].tableData) {
                if (row.size() < 4)
                    continue;
                auto& idxMap = tableIndexMap[row[0]];
                auto it = idxMap.find(row[1]);
                if (it == idxMap.end()) {
                    Index idx;
                    idx.name = row[1];
                    idx.isUnique = (row[2] == "UNIQUE");
                    it = idxMap.emplace(row[1], std::move(idx)).first;
                }
                it->second.columns.push_back(row[3]);
            }
            for (auto& [tblName, idxMap] : tableIndexMap) {
                for (auto& [idxName, idx] : idxMap) {
                    allIndexes[tblName].push_back(std::move(idx));
                }
            }
        }
    }

    for (const auto& tn : tableNames) {
        Table t;
        t.name = tn;
        t.schema = schema_;
        t.fullName = connName_ + "." + schema_ + "." + tn;
        if (auto it = allColumns.find(tn); it != allColumns.end())
            t.columns = std::move(it->second);
        if (auto it = allPKs.find(tn); it != allPKs.end()) {
            for (const auto& pkCol : it->second)
                for (auto& c : t.columns)
                    if (c.name == pkCol)
                        c.isPrimaryKey = true;
        }
        if (auto it = allFKs.find(tn); it != allFKs.end())
            t.foreignKeys = std::move(it->second);
        if (auto it = allIndexes.find(tn); it != allIndexes.end())
            t.indexes = std::move(it->second);
        buildForeignKeyLookup(t);
        result.push_back(std::move(t));
    }
    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> OracleDatabase::views() {
    std::vector<Table> result;
    std::lock_guard lock(*connMutex_);
    auto viewNames = queryStringList(
        ctx_, conn_,
        std::format("SELECT VIEW_NAME FROM ALL_VIEWS WHERE OWNER = '{}' ORDER BY VIEW_NAME",
                    escapeLiteral(schema_)));
    for (const auto& vn : viewNames) {
        Table v;
        v.name = vn;
        v.schema = schema_;
        v.fullName = connName_ + "." + schema_ + "." + vn;
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<std::string> OracleDatabase::sequences() {
    std::lock_guard lock(*connMutex_);
    return queryStringList(ctx_, conn_,
                           std::format("SELECT SEQUENCE_NAME FROM ALL_SEQUENCES "
                                       "WHERE SEQUENCE_OWNER = '{}' ORDER BY SEQUENCE_NAME",
                                       escapeLiteral(schema_)));
}

std::vector<Routine> OracleDatabase::routines() {
    std::vector<Routine> out;
    std::lock_guard lock(*connMutex_);
    std::string q = std::format(
        "SELECT o.OBJECT_NAME, "
        "o.OBJECT_NAME || '(' || NVL("
        "(SELECT LISTAGG(a.ARGUMENT_NAME || ' ' || a.DATA_TYPE, ', ') "
        "WITHIN GROUP (ORDER BY a.POSITION) FROM ALL_ARGUMENTS a "
        "WHERE a.OWNER = o.OWNER AND a.OBJECT_NAME = o.OBJECT_NAME AND a.POSITION > 0), '') "
        "|| ')' AS signature, o.OBJECT_TYPE, "
        "NVL((SELECT a.DATA_TYPE FROM ALL_ARGUMENTS a "
        "WHERE a.OWNER = o.OWNER AND a.OBJECT_NAME = o.OBJECT_NAME AND a.POSITION = 0), '') "
        "AS return_type "
        "FROM ALL_OBJECTS o WHERE o.OWNER = '{}' AND o.OBJECT_TYPE IN ('FUNCTION', 'PROCEDURE') "
        "ORDER BY o.OBJECT_TYPE, o.OBJECT_NAME",
        escapeLiteral(schema_));
    auto r = runQuery(ctx_, conn_, q, 10000);
    if (!r.success() || r.empty())
        return out;
    for (const auto& row : r[0].tableData) {
        if (row.size() < 4)
            continue;
        Routine rt;
        rt.name = row[0];
        rt.signature = row[1];
        rt.kind = (row[2] == "FUNCTION") ? RoutineKind::Function : RoutineKind::Procedure;
        rt.returnType = row[3];
        out.push_back(std::move(rt));
    }
    return out;
}

Table OracleDatabase::describeTable(const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.schema = schema_;
    t.fullName = connName_ + "." + schema_ + "." + tableName;

    std::lock_guard lock(*connMutex_);

    auto cols = runQuery(
        ctx_, conn_,
        std::format("SELECT COLUMN_NAME, DATA_TYPE, NULLABLE, DATA_LENGTH, DATA_PRECISION, "
                    "DATA_SCALE, IDENTITY_COLUMN FROM ALL_TAB_COLUMNS "
                    "WHERE OWNER = '{}' AND TABLE_NAME = '{}' ORDER BY COLUMN_ID",
                    escapeLiteral(schema_), escapeLiteral(tableName)),
        100000);
    if (cols.success() && !cols.empty()) {
        for (const auto& row : cols[0].tableData) {
            if (row.size() < 7)
                continue;
            Column c;
            c.name = row[0];
            const std::string& dataType = row[1];
            c.isNotNull = (row[2] == "N");
            const std::string& dataLen = row[3];
            const std::string& prec = row[4];
            const std::string& scale = row[5];
            c.isAutoIncrement = (row[6] == "YES");
            if (!isNullSentinel(prec)) {
                if (!isNullSentinel(scale) && scale != "0")
                    c.type = std::format("{}({},{})", dataType, prec, scale);
                else
                    c.type = std::format("{}({})", dataType, prec);
            } else if (dataType == "VARCHAR2" || dataType == "CHAR" || dataType == "NVARCHAR2" ||
                       dataType == "RAW") {
                c.type = std::format("{}({})", dataType, dataLen);
            } else {
                c.type = dataType;
            }
            t.columns.push_back(std::move(c));
        }
    }

    auto pkCols = queryStringList(
        ctx_, conn_,
        std::format("SELECT acc.COLUMN_NAME FROM ALL_CONS_COLUMNS acc "
                    "JOIN ALL_CONSTRAINTS ac ON acc.CONSTRAINT_NAME = ac.CONSTRAINT_NAME "
                    "AND acc.OWNER = ac.OWNER "
                    "WHERE ac.CONSTRAINT_TYPE = 'P' AND ac.TABLE_NAME = '{}' AND ac.OWNER = '{}'",
                    escapeLiteral(tableName), escapeLiteral(schema_)));
    for (const auto& pkCol : pkCols)
        for (auto& c : t.columns)
            if (c.name == pkCol)
                c.isPrimaryKey = true;

    auto fkRes = runQuery(
        ctx_, conn_,
        std::format("SELECT ac.CONSTRAINT_NAME, acc.COLUMN_NAME, rc.TABLE_NAME, rcc.COLUMN_NAME "
                    "FROM ALL_CONSTRAINTS ac "
                    "JOIN ALL_CONS_COLUMNS acc ON ac.CONSTRAINT_NAME = acc.CONSTRAINT_NAME "
                    "AND ac.OWNER = acc.OWNER "
                    "JOIN ALL_CONSTRAINTS rc ON ac.R_CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
                    "AND ac.R_OWNER = rc.OWNER "
                    "JOIN ALL_CONS_COLUMNS rcc ON rc.CONSTRAINT_NAME = rcc.CONSTRAINT_NAME "
                    "AND rc.OWNER = rcc.OWNER AND acc.POSITION = rcc.POSITION "
                    "WHERE ac.CONSTRAINT_TYPE = 'R' AND ac.TABLE_NAME = '{}' AND ac.OWNER = '{}' "
                    "ORDER BY ac.CONSTRAINT_NAME, acc.POSITION",
                    escapeLiteral(tableName), escapeLiteral(schema_)),
        100000);
    if (fkRes.success() && !fkRes.empty()) {
        for (const auto& row : fkRes[0].tableData) {
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
    buildForeignKeyLookup(t);
    return t;
}

std::vector<std::vector<std::string>>
OracleDatabase::getTableData(const Table& table, int limit, int offset,
                             const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> data;
    std::lock_guard lock(*connMutex_);

    std::string sql = "SELECT * FROM " + qualifiedTable(table);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    // Oracle's OFFSET/FETCH NEXT requires ORDER BY for stable paging
    if (!orderByClause.empty())
        sql += " ORDER BY " + orderByClause;
    sql += std::format(" OFFSET {} ROWS FETCH NEXT {} ROWS ONLY", offset, limit);

    auto r = runQuery(ctx_, conn_, sql, limit);
    if (!r.success() || r.empty())
        return data;
    return r[0].tableData;
}

std::vector<std::string> OracleDatabase::getColumnNames(const Table& table) {
    std::lock_guard lock(*connMutex_);
    const std::string sch = table.schema.empty() ? schema_ : table.schema;
    return queryStringList(
        ctx_, conn_,
        std::format("SELECT COLUMN_NAME FROM ALL_TAB_COLUMNS "
                    "WHERE OWNER = '{}' AND TABLE_NAME = '{}' ORDER BY COLUMN_ID",
                    escapeLiteral(sch), escapeLiteral(table.name)));
}

int OracleDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    std::lock_guard lock(*connMutex_);
    std::string sql = "SELECT COUNT(*) FROM " + qualifiedTable(table);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    auto s = queryScalar(ctx_, conn_, sql);
    if (s.empty() || isNullSentinel(s))
        return 0;
    try {
        return std::stoi(s);
    } catch (...) {
        return 0;
    }
}

Status OracleDatabase::createTable(const Table& table) {
    std::lock_guard lock(*connMutex_);
    std::string sql = "CREATE TABLE " + qualifiedTable(table) + " (";
    bool first = true;
    for (const auto& c : table.columns) {
        if (!first)
            sql += ", ";
        first = false;
        sql += quoteIdent(c.name) + " " + (c.type.empty() ? std::string("VARCHAR2(255)") : c.type);
        if (c.isNotNull && !c.isPrimaryKey)
            sql += " NOT NULL";
        if (c.isUnique && !c.isPrimaryKey)
            sql += " UNIQUE";
        if (!c.defaultValue.empty())
            sql += " DEFAULT " + c.defaultValue;
        if (c.isPrimaryKey)
            sql += " PRIMARY KEY";
    }
    for (const auto& fk : table.foreignKeys) {
        sql += std::format(", FOREIGN KEY ({}) REFERENCES {}({})", quoteIdent(fk.sourceColumn),
                           quoteIdent(fk.targetTable), quoteIdent(fk.targetColumn));
    }
    sql += ")";
    auto r = execLocked(sql);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status OracleDatabase::renameTable(const std::string& oldName, const std::string& newName) {
    std::lock_guard lock(*connMutex_);
    auto r = execLocked(std::format("ALTER TABLE {} RENAME TO {}", qualifiedTable(oldName),
                                    quoteIdent(newName)));
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status OracleDatabase::dropTable(const std::string& tableName) {
    std::lock_guard lock(*connMutex_);
    auto r = execLocked("DROP TABLE " + qualifiedTable(tableName));
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status OracleDatabase::truncateTable(const std::string& tableName) {
    std::lock_guard lock(*connMutex_);
    auto r = execLocked("TRUNCATE TABLE " + qualifiedTable(tableName));
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status OracleDatabase::dropColumn(const std::string& tableName, const std::string& columnName) {
    std::lock_guard lock(*connMutex_);
    auto r = execLocked(std::format("ALTER TABLE {} DROP COLUMN {}", qualifiedTable(tableName),
                                    quoteIdent(columnName)));
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status OracleDatabase::dropView(const std::string& viewName, bool isMaterialized) {
    std::lock_guard lock(*connMutex_);
    const char* kw = isMaterialized ? "MATERIALIZED VIEW" : "VIEW";
    auto r = execLocked(std::format("DROP {} {}.{}", kw, quoteIdent(schema_), quoteIdent(viewName)));
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

// ---------- ConnectionImpl ----------

class ConnectionImpl {
public:
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {
        // Oracle sslmode "prefer"/"allow" map to disable for our purposes
        if (info_.sslmode == SslMode::Prefer || info_.sslmode == SslMode::Allow)
            info_.sslmode = SslMode::Disable;
    }

    ~ConnectionImpl() {
        close();
    }

    Status open() {
        if (conn_)
            return {true, ""};
        ctx_ = getDpiContext();
        if (!ctx_) {
            std::string err = lastInitError();
            return {false, err.empty() ? "Failed to initialize ODPI-C context" : err};
        }

        try {
            if (info_.database.empty()) {
                for (const auto& candidate : {"FREEPDB1", "XEPDB1", "XE", "ORCL", "FREE"}) {
                    ConnectionInfo probeInfo = info_;
                    probeInfo.database = candidate;
                    const auto probeStr = buildConnectString(probeInfo);
                    dpiConn* probe = nullptr;
                    if (dpiConn_create(ctx_, probeInfo.username.c_str(),
                                       static_cast<uint32_t>(probeInfo.username.size()),
                                       probeInfo.password.c_str(),
                                       static_cast<uint32_t>(probeInfo.password.size()),
                                       probeStr.c_str(), static_cast<uint32_t>(probeStr.size()),
                                       nullptr, nullptr, &probe) == DPI_SUCCESS) {
                        dpiConn_release(probe);
                        info_.database = candidate;
                        break;
                    }
                }
                if (info_.database.empty()) {
                    return {false,
                            "Could not find a valid Oracle service. Try specifying the service "
                            "name."};
                }
            }

            std::string connStr = buildConnectString(info_);
            dpiConn* c = nullptr;
            if (dpiConn_create(ctx_, info_.username.c_str(),
                               static_cast<uint32_t>(info_.username.size()), info_.password.c_str(),
                               static_cast<uint32_t>(info_.password.size()), connStr.c_str(),
                               static_cast<uint32_t>(connStr.size()), nullptr, nullptr,
                               &c) != DPI_SUCCESS) {
                return {false, "Oracle connection failed: " + dpiErrText(ctx_)};
            }
            conn_ = c;
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
        defaultSchema_ = toUpper(info_.username);
        return {true, ""};
    }

    void close() {
        cache_.clear();
        if (conn_) {
            dpiConn_release(conn_);
            conn_ = nullptr;
        }
    }

    bool isOpen() const {
        return conn_ != nullptr;
    }

    const ConnectionInfo& info() const {
        return info_;
    }

    std::vector<DatabasePtr> databases() {
        if (!conn_)
            return {};
        std::vector<std::string> users;
        {
            std::lock_guard lock(connMutex_);
            users = queryStringList(ctx_, conn_,
                                    "SELECT USERNAME FROM ALL_USERS ORDER BY USERNAME");
        }
        std::vector<DatabasePtr> out;
        out.reserve(users.size());
        for (const auto& u : users)
            out.push_back(database(u));
        return out;
    }

    DatabasePtr database(const std::string& name) {
        if (!conn_)
            return nullptr;
        const std::string n = name.empty() ? defaultSchema_ : toUpper(name);
        auto it = cache_.find(n);
        if (it != cache_.end())
            return it->second;
        auto db = std::make_shared<OracleDatabase>(ctx_, conn_, connMutex_, info_.name, n, false);
        cache_[n] = db;
        return db;
    }

    Status createDatabase(const CreateDatabaseOptions& opts) {
        if (!conn_)
            return {false, "Not connected"};
        if (opts.name.empty())
            return {false, "Schema name required"};
        std::lock_guard lock(connMutex_);
        const std::string pwd = opts.owner.empty() ? opts.name : opts.owner;
        auto r = runQuery(
            ctx_, conn_,
            std::format("CREATE USER {} IDENTIFIED BY \"{}\"", quoteIdent(toUpper(opts.name)), pwd),
            0);
        if (!r.success())
            return {false, r.errorMessage()};
        runQuery(ctx_, conn_,
                 std::format("GRANT CONNECT, RESOURCE TO {}", quoteIdent(toUpper(opts.name))), 0);
        return {true, ""};
    }

    Status dropDatabase(const std::string& name) {
        if (!conn_)
            return {false, "Not connected"};
        const std::string n = toUpper(name);
        cache_.erase(n);
        std::lock_guard lock(connMutex_);
        auto r = runQuery(ctx_, conn_, std::format("DROP USER {} CASCADE", quoteIdent(n)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    static std::string oracleWalletLocation(const std::string& path) {
        if (path.empty())
            return {};

        std::error_code ec;
        std::filesystem::path walletPath(path);
        if (std::filesystem::is_regular_file(walletPath, ec)) {
            auto parent = walletPath.parent_path();
            if (!parent.empty())
                return parent.string();
        }
        return walletPath.string();
    }

    static std::string buildConnectString(const ConnectionInfo& info) {
        const bool useTls = info.sslmode == SslMode::Require ||
                            info.sslmode == SslMode::VerifyCA ||
                            info.sslmode == SslMode::VerifyFull;
        const bool needsWallet =
            info.sslmode == SslMode::VerifyCA || info.sslmode == SslMode::VerifyFull;
        if (!useTls)
            return std::format("{}:{}/{}", info.host, info.port, info.database);
        std::string s = std::format("tcps://{}:{}/{}", info.host, info.port, info.database);
        std::vector<std::string> params;
        if (info.sslmode == SslMode::Require)
            params.emplace_back("ssl_server_dn_match=off");
        if (needsWallet) {
            auto walletLocation = oracleWalletLocation(info.sslCACertPath);
            if (walletLocation.empty()) {
                throw std::runtime_error(
                    "Oracle TLS verify mode requires a wallet path or wallet file location");
            }
            params.push_back(std::format("wallet_location=\"{}\"", walletLocation));
        }
        if (!params.empty()) {
            s += '?';
            for (size_t i = 0; i < params.size(); ++i) {
                if (i)
                    s += '&';
                s += params[i];
            }
        }
        return s;
    }

    ConnectionInfo info_;
    dpiContext* ctx_ = nullptr;
    dpiConn* conn_ = nullptr;
    std::mutex connMutex_;
    std::string defaultSchema_;
    std::unordered_map<std::string, std::shared_ptr<OracleDatabase>> cache_;
};

} // namespace

// ---------- OracleConnection façade ----------

OracleConnection::OracleConnection(const ConnectionInfo& info) : info_(info) {
    impl_ = new ConnectionImpl(info_);
}

OracleConnection::~OracleConnection() {
    delete static_cast<ConnectionImpl*>(impl_);
}

Status OracleConnection::open() {
    auto* impl = static_cast<ConnectionImpl*>(impl_);
    auto s = impl->open();
    info_ = impl->info();
    return s;
}

void OracleConnection::close() {
    static_cast<ConnectionImpl*>(impl_)->close();
}

bool OracleConnection::isOpen() const {
    return static_cast<const ConnectionImpl*>(impl_)->isOpen();
}

std::vector<DatabasePtr> OracleConnection::databases() {
    return static_cast<ConnectionImpl*>(impl_)->databases();
}

DatabasePtr OracleConnection::database(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->database(name);
}

} // namespace dearsql
