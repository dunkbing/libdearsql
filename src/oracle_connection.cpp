#include "dearsql/backends/oracle_connection.hpp"
#include "dearsql/oracle_installer.hpp"

#if defined(__linux__)
#include <dlfcn.h>
#include <fstream>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dpi.h>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dearsql {

namespace {

// ---------- ODPI-C global context ----------

struct ContextState {
    std::mutex mutex;
    dpiContext* ctx = nullptr;
    std::string error;
    bool failed = false;
    bool failedInstalled = false; // the client was present when it failed
    bool autoInstallAttempted = false;
    oracle::ClientOptions options;
    std::string libDir; // kept alive for ODPI's lifetime
};

ContextState& ctxState() {
    static ContextState s;
    return s;
}


#if defined(__linux__)
// Returns true if LD_LIBRARY_PATH already contains `dir` as a colon-separated
// entry.
bool ldLibraryPathContains(const std::string& dir) {
    const char* env = std::getenv("LD_LIBRARY_PATH");
    if (!env || !*env)
        return false;
    std::string_view sv = env;
    size_t start = 0;
    while (start <= sv.size()) {
        size_t end = sv.find(':', start);
        if (end == std::string_view::npos)
            end = sv.size();
        if (sv.substr(start, end - start) == dir)
            return true;
        if (end == sv.size())
            break;
        start = end + 1;
    }
    return false;
}

// Re-exec the current binary with LD_LIBRARY_PATH=<installDir>:<existing>.
//
// Oracle Instant Client's libclntsh.so DT_NEEDEDs libnnz.so and
// libclntshcore.so.<v>, both bundled in the install dir. libclntsh ships with
// no $ORIGIN RUNPATH; libnnz ships with no DT_SONAME, so the dlopen +
// RTLD_GLOBAL preload trick doesn't satisfy DT_NEEDED resolution either. The
// only portable way to make ld.so find the bundled deps is via the standard
// search path, which it caches at process start. setenv() after main() won't
// affect that cache, so we set the env and exec(2) ourselves with the same
// argv. Guarded by DEARSQL_ORACLE_LD_BOOTSTRAPPED to avoid loops.
[[nodiscard]] bool reexecWithLdLibraryPath(const std::string& installDir) {
    if (std::getenv("DEARSQL_ORACLE_LD_BOOTSTRAPPED"))
        return false; // already attempted once; don't loop
    setenv("DEARSQL_ORACLE_LD_BOOTSTRAPPED", "1", 1);

    std::string newPath = installDir;
    if (const char* cur = std::getenv("LD_LIBRARY_PATH"); cur && *cur) {
        newPath += ':';
        newPath += cur;
    }
    setenv("LD_LIBRARY_PATH", newPath.c_str(), 1);

    // Reconstruct argv from /proc/self/cmdline (null-separated).
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    if (!cmdline)
        return false;
    std::vector<std::string> args;
    std::string acc;
    for (char c; cmdline.get(c);) {
        if (c == '\0') {
            args.push_back(std::move(acc));
            acc.clear();
        } else {
            acc += c;
        }
    }
    if (args.empty())
        return false;

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    std::fflush(stdout);
    std::fflush(stderr);
    execv("/proc/self/exe", argv.data());
    return false; // execv only returns on failure
}
#endif

dpiContext* getDpiContext(bool allowInstall) {
    auto& s = ctxState();
    std::lock_guard lock(s.mutex);
    if (s.ctx)
        return s.ctx;

    bool installed = oracle::isInstalled();
    if (!installed && allowInstall && s.options.autoInstall && !s.autoInstallAttempted) {
        s.autoInstallAttempted = true;
        auto [ok, err] = oracle::install();
        if (!ok) {
            s.error = "Oracle Instant Client auto-install failed: " +
                      (err.empty() ? std::string("unknown error") : err);
            s.failed = true;
            s.failedInstalled = false;
            return nullptr;
        }
        installed = oracle::isInstalled();
    }
    // nothing changed since the last failure
    if (s.failed && s.failedInstalled == installed)
        return nullptr;

    dpiContextCreateParams params{};
    if (installed) {
        s.libDir = oracle::installDir();
        params.oracleClientLibDir = s.libDir.c_str();
#if defined(__linux__)
        // a prior install may have placed libclntsh but failed to fetch libaio
        oracle::ensureLibaio(s.libDir);
        // glibc matches already-loaded libraries by SONAME when resolving
        // libclntsh's DT_NEEDED, so a global preload satisfies libaio.so.1
        const std::string libaio = s.libDir + "/libaio.so.1";
        dlopen(libaio.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (s.options.reexecForLibraryPath && !ldLibraryPathContains(s.libDir)) {
            // only returns on failure; context creation reports the error
            (void)reexecWithLdLibraryPath(s.libDir);
        }
#endif
    }

    dpiErrorInfo errorInfo;
    if (dpiContext_createWithParams(DPI_MAJOR_VERSION, DPI_MINOR_VERSION, &params, &s.ctx,
                                    &errorInfo) != DPI_SUCCESS) {
        std::string msg(errorInfo.message, errorInfo.messageLength);
        s.error = msg.find("DPI-1047") != std::string::npos
                      ? "Oracle Instant Client is not installed."
                      : msg;
        s.ctx = nullptr;
        s.failed = true;
        s.failedInstalled = installed;
        return nullptr;
    }
    s.failed = false;
    s.error.clear();
    return s.ctx;
}

std::string lastInitError() {
    auto& s = ctxState();
    std::lock_guard lock(s.mutex);
    return s.error.empty() ? "Failed to initialize ODPI-C context" : s.error;
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

StatementResult failed(std::string message) {
    StatementResult r;
    r.success = false;
    r.errorMessage = std::move(message);
    return r;
}

// one statement; rowLimit <= 0 fetches everything
QueryResult runQuery(dpiContext* ctx, dpiConn* conn, const std::string& query, int rowLimit) {
    QueryResult result;
    const auto startTime = std::chrono::steady_clock::now();
    auto finish = [&] {
        result.executionTimeMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime)
                .count();
        return result;
    };

    dpiStmt* raw = nullptr;
    if (dpiConn_prepareStmt(conn, 0, query.c_str(), static_cast<uint32_t>(query.size()), nullptr, 0,
                            &raw) != DPI_SUCCESS) {
        result.statements.push_back(failed(dpiErrText(ctx)));
        return finish();
    }
    StmtPtr stmt(raw);

    dpiStmtInfo stmtInfo;
    dpiStmt_getInfo(raw, &stmtInfo);
    const dpiExecMode execMode =
        stmtInfo.isQuery ? DPI_MODE_EXEC_DEFAULT : DPI_MODE_EXEC_COMMIT_ON_SUCCESS;

    uint32_t numCols = 0;
    if (dpiStmt_execute(raw, execMode, &numCols) != DPI_SUCCESS) {
        result.statements.push_back(failed(dpiErrText(ctx)));
        return finish();
    }

    StatementResult r;
    if (numCols > 0) {
        for (uint32_t i = 1; i <= numCols; i++) {
            dpiQueryInfo qi;
            dpiStmt_getQueryInfo(raw, i, &qi);
            r.columnNames.emplace_back(qi.name, qi.nameLength);
            // CLOBs as text (DBMS_METADATA, long columns), not lob locators
            const auto ot = qi.typeInfo.oracleTypeNum;
            if (ot == DPI_ORACLE_TYPE_CLOB || ot == DPI_ORACLE_TYPE_NCLOB)
                dpiStmt_defineValue(raw, i, DPI_ORACLE_TYPE_LONG_VARCHAR, DPI_NATIVE_TYPE_BYTES,
                                    0, 0, nullptr);
        }
        int found = 0;
        uint32_t bufIdx = 0;
        while ((rowLimit <= 0 || static_cast<int>(r.tableData.size()) < rowLimit) &&
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
        }
        r.message = std::format("Returned {} row{}", r.tableData.size(),
                                r.tableData.size() == 1 ? "" : "s");
        if (rowLimit > 0 && static_cast<int>(r.tableData.size()) >= rowLimit)
            r.message += std::format(" (limited to {})", rowLimit);
    } else {
        uint64_t rowsAffected = 0;
        dpiStmt_getRowCount(raw, &rowsAffected);
        r.affectedRows = static_cast<int>(rowsAffected);
        r.message = rowsAffected > 0 ? std::format("{} row(s) affected", rowsAffected)
                                     : "Query executed successfully";
    }
    result.statements.push_back(std::move(r));
    return finish();
}

using Rows = std::vector<std::vector<std::string>>;

// catalog query; throws on failure
Rows queryRows(dpiContext* ctx, dpiConn* conn, const std::string& sql) {
    auto r = runQuery(ctx, conn, sql, 0);
    if (!r.success())
        throw Error(r.errorMessage());
    return std::move(r.statements.front().tableData);
}

std::vector<std::string> firstColumn(const Rows& rows) {
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const auto& row : rows) {
        if (!row.empty() && !isNullSentinel(row[0]))
            out.push_back(row[0]);
    }
    return out;
}

std::string walletLocation(const std::string& path) {
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

std::string buildConnectString(const ConnectionInfo& info) {
    const bool useTls = info.sslmode == SslMode::Require || info.sslmode == SslMode::VerifyCA ||
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
        auto location = walletLocation(info.sslCACertPath);
        if (location.empty())
            throw Error("Oracle TLS verify mode requires a wallet path or wallet file location");
        params.push_back(std::format("wallet_location=\"{}\"", location));
    }
    for (size_t i = 0; i < params.size(); ++i)
        s += (i ? '&' : '?') + params[i];
    return s;
}

// a new session; throws on failure
dpiConn* createConn(dpiContext* ctx, const ConnectionInfo& info) {
    const auto connStr = buildConnectString(info);
    dpiConn* conn = nullptr;
    if (dpiConn_create(ctx, info.username.c_str(), static_cast<uint32_t>(info.username.size()),
                       info.password.c_str(), static_cast<uint32_t>(info.password.size()),
                       connStr.c_str(), static_cast<uint32_t>(connStr.size()), nullptr, nullptr,
                       &conn) != DPI_SUCCESS)
        throw Error("Oracle connection failed: " + dpiErrText(ctx));
    return conn;
}

std::string columnType(const std::string& dataType, const std::string& dataLen,
                       const std::string& prec, const std::string& scale) {
    if (!isNullSentinel(prec)) {
        if (!isNullSentinel(scale) && scale != "0")
            return std::format("{}({},{})", dataType, prec, scale);
        return std::format("{}({})", dataType, prec);
    }
    if (dataType == "VARCHAR2" || dataType == "CHAR" || dataType == "NVARCHAR2" ||
        dataType == "RAW")
        return std::format("{}({})", dataType, dataLen);
    return dataType;
}

// ---------- OracleDatabase: one schema (owner) on its own session ----------

class OracleDatabase final : public IDatabase {
public:
    OracleDatabase(dpiContext* ctx, ConnectionInfo info, std::string schema)
        : ctx_(ctx), info_(std::move(info)), schema_(std::move(schema)) {}

    ~OracleDatabase() override {
        if (auto* c = conn_.exchange(nullptr))
            dpiConn_release(c);
    }

    [[nodiscard]] std::string name() const override {
        return schema_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::ORACLE;
    }
    [[nodiscard]] std::string schemaName() const override {
        return schema_;
    }

    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    std::vector<std::string> sequences() override;
    std::vector<Routine> routines() override;
    Table describeTable(const std::string& tableName) override;

    QueryResult execute(const std::string& sql, int rowLimit) override {
        std::lock_guard lock(mu_);
        try {
            return runQuery(ctx_, ensureConn(), sql, rowLimit);
        } catch (const std::exception& e) {
            QueryResult r;
            r.statements.push_back(failed(e.what()));
            return r;
        }
    }

    bool alive() override {
        std::lock_guard lock(mu_);
        auto* c = conn_.load();
        return !c || dpiConn_ping(c) == DPI_SUCCESS;
    }

    // OCIBreak on the running call; safe from another thread
    void cancel() override {
        if (auto* c = conn_.load())
            dpiConn_breakExecution(c);
    }

    // builder default needs an owner for ALL_TAB_COLUMNS
    std::vector<std::string> getColumnNames(const Table& table) override {
        if (!table.schema.empty())
            return IDatabase::getColumnNames(table);
        Table t = table;
        t.schema = schema_;
        return IDatabase::getColumnNames(t);
    }

private:
    // the session, opened on first use with CURRENT_SCHEMA pointing here so
    // unqualified SQL resolves against this schema; caller holds mu_
    dpiConn* ensureConn() {
        if (auto* c = conn_.load())
            return c;
        dpiConn* c = createConn(ctx_, info_);
        if (schema_ != toUpper(info_.username)) {
            auto r = runQuery(ctx_, c,
                              "ALTER SESSION SET CURRENT_SCHEMA = " + quoteIdent(schema_), 0);
            if (!r.success()) {
                dpiConn_release(c);
                throw Error(r.errorMessage());
            }
        }
        conn_.store(c);
        return c;
    }

    Rows query(const std::string& sql) {
        return queryRows(ctx_, ensureConn(), sql);
    }

    std::string owner() const {
        return escapeLiteral(schema_);
    }

    // per-table metadata; tableName empty = the whole schema. caller holds mu_
    std::unordered_map<std::string, std::vector<Column>> columns(const std::string& filter);
    std::unordered_map<std::string, std::vector<std::string>>
    primaryKeys(const std::string& tableName);
    std::unordered_map<std::string, std::vector<ForeignKey>>
    foreignKeys(const std::string& tableName);
    std::unordered_map<std::string, std::vector<Index>> indexes(const std::string& tableName);

    dpiContext* ctx_;
    ConnectionInfo info_;
    std::string schema_;
    std::atomic<dpiConn*> conn_{nullptr};
    std::mutex mu_;
};

std::unordered_map<std::string, std::vector<Column>>
OracleDatabase::columns(const std::string& filter) {
    std::unordered_map<std::string, std::vector<Column>> out;
    for (const auto& row :
         query(std::format("SELECT TABLE_NAME, COLUMN_NAME, DATA_TYPE, NULLABLE, DATA_LENGTH, "
                           "DATA_PRECISION, DATA_SCALE, IDENTITY_COLUMN FROM ALL_TAB_COLUMNS "
                           "WHERE OWNER = '{}'{} ORDER BY TABLE_NAME, COLUMN_ID",
                           owner(), filter))) {
        if (row.size() < 8)
            continue;
        Column c;
        c.name = row[1];
        c.type = columnType(row[2], row[4], row[5], row[6]);
        c.isNotNull = row[3] == "N";
        c.isAutoIncrement = row[7] == "YES";
        out[row[0]].push_back(std::move(c));
    }
    return out;
}

std::unordered_map<std::string, std::vector<std::string>>
OracleDatabase::primaryKeys(const std::string& tableName) {
    std::unordered_map<std::string, std::vector<std::string>> out;
    const std::string filter =
        tableName.empty() ? "" : std::format(" AND ac.TABLE_NAME = '{}'", escapeLiteral(tableName));
    for (const auto& row :
         query(std::format("SELECT ac.TABLE_NAME, acc.COLUMN_NAME FROM ALL_CONS_COLUMNS acc "
                           "JOIN ALL_CONSTRAINTS ac ON acc.CONSTRAINT_NAME = ac.CONSTRAINT_NAME "
                           "AND acc.OWNER = ac.OWNER "
                           "WHERE ac.CONSTRAINT_TYPE = 'P' AND ac.OWNER = '{}'{}",
                           owner(), filter))) {
        if (row.size() >= 2)
            out[row[0]].push_back(row[1]);
    }
    return out;
}

std::unordered_map<std::string, std::vector<ForeignKey>>
OracleDatabase::foreignKeys(const std::string& tableName) {
    std::unordered_map<std::string, std::vector<ForeignKey>> out;
    const std::string filter =
        tableName.empty() ? "" : std::format(" AND ac.TABLE_NAME = '{}'", escapeLiteral(tableName));
    for (const auto& row :
         query(std::format("SELECT ac.TABLE_NAME, ac.CONSTRAINT_NAME, acc.COLUMN_NAME, "
                           "rc.TABLE_NAME, rcc.COLUMN_NAME "
                           "FROM ALL_CONSTRAINTS ac "
                           "JOIN ALL_CONS_COLUMNS acc ON ac.CONSTRAINT_NAME = acc.CONSTRAINT_NAME "
                           "AND ac.OWNER = acc.OWNER "
                           "JOIN ALL_CONSTRAINTS rc ON ac.R_CONSTRAINT_NAME = rc.CONSTRAINT_NAME "
                           "AND ac.R_OWNER = rc.OWNER "
                           "JOIN ALL_CONS_COLUMNS rcc ON rc.CONSTRAINT_NAME = rcc.CONSTRAINT_NAME "
                           "AND rc.OWNER = rcc.OWNER AND acc.POSITION = rcc.POSITION "
                           "WHERE ac.CONSTRAINT_TYPE = 'R' AND ac.OWNER = '{}'{} "
                           "ORDER BY ac.TABLE_NAME, ac.CONSTRAINT_NAME, acc.POSITION",
                           owner(), filter))) {
        if (row.size() < 5)
            continue;
        ForeignKey fk;
        fk.name = row[1];
        fk.sourceColumn = row[2];
        fk.targetTable = row[3];
        fk.targetColumn = row[4];
        out[row[0]].push_back(std::move(fk));
    }
    return out;
}

// primary key indexes are left out; the columns carry them
std::unordered_map<std::string, std::vector<Index>>
OracleDatabase::indexes(const std::string& tableName) {
    const std::string filter =
        tableName.empty() ? "" : std::format(" AND i.TABLE_NAME = '{}'", escapeLiteral(tableName));
    std::unordered_map<std::string, std::map<std::string, Index>> byTable;
    for (const auto& row :
         query(std::format("SELECT i.TABLE_NAME, i.INDEX_NAME, i.UNIQUENESS, ic.COLUMN_NAME "
                           "FROM ALL_INDEXES i "
                           "JOIN ALL_IND_COLUMNS ic ON i.INDEX_NAME = ic.INDEX_NAME "
                           "AND i.OWNER = ic.INDEX_OWNER "
                           "LEFT JOIN ALL_CONSTRAINTS c ON i.INDEX_NAME = c.INDEX_NAME "
                           "AND i.OWNER = c.OWNER "
                           "WHERE i.OWNER = '{}'{} "
                           "AND (c.CONSTRAINT_TYPE IS NULL OR c.CONSTRAINT_TYPE != 'P') "
                           "ORDER BY i.TABLE_NAME, i.INDEX_NAME, ic.COLUMN_POSITION",
                           owner(), filter))) {
        if (row.size() < 4)
            continue;
        auto& idx = byTable[row[0]][row[1]];
        idx.name = row[1];
        idx.isUnique = row[2] == "UNIQUE";
        idx.columns.push_back(row[3]);
    }
    std::unordered_map<std::string, std::vector<Index>> out;
    for (auto& [table, byName] : byTable) {
        for (auto& idx : byName)
            out[table].push_back(std::move(idx.second));
    }
    return out;
}

std::vector<Table> OracleDatabase::tables() {
    std::lock_guard lock(mu_);
    auto names = firstColumn(query(std::format(
        "SELECT TABLE_NAME FROM ALL_TABLES WHERE OWNER = '{}' ORDER BY TABLE_NAME", owner())));
    if (names.empty())
        return {};

    auto cols = columns("");
    auto pks = primaryKeys("");
    auto fks = foreignKeys("");
    auto idx = indexes("");

    std::vector<Table> result;
    result.reserve(names.size());
    for (const auto& tn : names) {
        Table t;
        t.name = tn;
        t.schema = schema_;
        t.fullName = info_.name + "." + schema_ + "." + tn;
        if (auto it = cols.find(tn); it != cols.end())
            t.columns = std::move(it->second);
        if (auto it = pks.find(tn); it != pks.end()) {
            for (auto& c : t.columns)
                c.isPrimaryKey = std::ranges::find(it->second, c.name) != it->second.end();
        }
        if (auto it = fks.find(tn); it != fks.end())
            t.foreignKeys = std::move(it->second);
        if (auto it = idx.find(tn); it != idx.end())
            t.indexes = std::move(it->second);
        buildForeignKeyLookup(t);
        result.push_back(std::move(t));
    }
    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> OracleDatabase::views() {
    std::lock_guard lock(mu_);
    auto names = firstColumn(query(std::format(
        "SELECT VIEW_NAME FROM ALL_VIEWS WHERE OWNER = '{}' ORDER BY VIEW_NAME", owner())));
    if (names.empty())
        return {};
    auto cols = columns(std::format(" AND TABLE_NAME IN (SELECT VIEW_NAME FROM ALL_VIEWS "
                                    "WHERE OWNER = '{}')",
                                    owner()));
    std::vector<Table> result;
    result.reserve(names.size());
    for (const auto& vn : names) {
        Table v;
        v.name = vn;
        v.schema = schema_;
        v.fullName = info_.name + "." + schema_ + "." + vn;
        if (auto it = cols.find(vn); it != cols.end())
            v.columns = std::move(it->second);
        result.push_back(std::move(v));
    }
    return result;
}

std::vector<std::string> OracleDatabase::sequences() {
    std::lock_guard lock(mu_);
    return firstColumn(query(std::format("SELECT SEQUENCE_NAME FROM ALL_SEQUENCES "
                                         "WHERE SEQUENCE_OWNER = '{}' ORDER BY SEQUENCE_NAME",
                                         owner())));
}

std::vector<Routine> OracleDatabase::routines() {
    std::lock_guard lock(mu_);
    std::vector<Routine> out;
    for (const auto& row : query(std::format(
             "SELECT o.OBJECT_NAME, "
             "o.OBJECT_NAME || '(' || NVL("
             "(SELECT LISTAGG(a.ARGUMENT_NAME || ' ' || a.DATA_TYPE, ', ') "
             "WITHIN GROUP (ORDER BY a.POSITION) FROM ALL_ARGUMENTS a "
             "WHERE a.OWNER = o.OWNER AND a.OBJECT_NAME = o.OBJECT_NAME AND a.POSITION > 0), '') "
             "|| ')' AS signature, o.OBJECT_TYPE, "
             "NVL((SELECT a.DATA_TYPE FROM ALL_ARGUMENTS a "
             "WHERE a.OWNER = o.OWNER AND a.OBJECT_NAME = o.OBJECT_NAME AND a.POSITION = 0), '') "
             "AS return_type "
             "FROM ALL_OBJECTS o WHERE o.OWNER = '{}' "
             "AND o.OBJECT_TYPE IN ('FUNCTION', 'PROCEDURE') "
             "ORDER BY o.OBJECT_TYPE, o.OBJECT_NAME",
             owner()))) {
        if (row.size() < 4)
            continue;
        Routine rt;
        rt.name = row[0];
        rt.signature = row[1];
        rt.kind = row[2] == "FUNCTION" ? RoutineKind::Function : RoutineKind::Procedure;
        // NVL(..., '') is NULL in Oracle
        rt.returnType = isNullSentinel(row[3]) ? "" : row[3];
        out.push_back(std::move(rt));
    }
    return out;
}

Table OracleDatabase::describeTable(const std::string& tableName) {
    std::lock_guard lock(mu_);
    Table t;
    t.name = tableName;
    t.schema = schema_;
    t.fullName = info_.name + "." + schema_ + "." + tableName;

    auto cols = columns(std::format(" AND TABLE_NAME = '{}'", escapeLiteral(tableName)));
    if (auto it = cols.find(tableName); it != cols.end())
        t.columns = std::move(it->second);
    auto pks = primaryKeys(tableName);
    if (auto it = pks.find(tableName); it != pks.end()) {
        for (auto& c : t.columns)
            c.isPrimaryKey = std::ranges::find(it->second, c.name) != it->second.end();
    }
    auto fks = foreignKeys(tableName);
    if (auto it = fks.find(tableName); it != fks.end())
        t.foreignKeys = std::move(it->second);
    auto idx = indexes(tableName);
    if (auto it = idx.find(tableName); it != idx.end())
        t.indexes = std::move(it->second);
    buildForeignKeyLookup(t);
    return t;
}

// ---------- ConnectionImpl ----------

class ConnectionImpl {
public:
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {
        // prefer/allow mean plain TCP here
        if (info_.sslmode == SslMode::Prefer || info_.sslmode == SslMode::Allow)
            info_.sslmode = SslMode::Disable;
    }

    ~ConnectionImpl() {
        close();
    }

    Status open() {
        std::lock_guard lock(connMutex_);
        if (conn_)
            return {true, ""};
        ctx_ = getDpiContext(true);
        if (!ctx_)
            return {false, lastInitError()};

        try {
            // no service name: probe the usual ones
            if (info_.database.empty()) {
                for (const auto* candidate : {"FREEPDB1", "XEPDB1", "XE", "ORCL", "FREE"}) {
                    ConnectionInfo probe = info_;
                    probe.database = candidate;
                    try {
                        dpiConn_release(createConn(ctx_, probe));
                        info_.database = candidate;
                        break;
                    } catch (const Error&) {
                    }
                }
                if (info_.database.empty())
                    return {false,
                            "Could not find a valid Oracle service. Try specifying the service "
                            "name."};
            }
            conn_ = createConn(ctx_, info_);
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
        defaultSchema_ = toUpper(info_.username);
        return {true, ""};
    }

    void close() {
        {
            std::lock_guard lock(cacheMutex_);
            cache_.clear();
        }
        std::lock_guard lock(connMutex_);
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

    // owners of user objects; system accounts stay hidden
    std::vector<DatabasePtr> databases() {
        std::vector<std::string> owners;
        {
            std::lock_guard lock(connMutex_);
            if (!conn_)
                throw Error("Not connected to Oracle");
            owners = firstColumn(queryRows(
                ctx_, conn_,
                "SELECT DISTINCT OWNER FROM ALL_OBJECTS "
                "WHERE OBJECT_TYPE IN ('TABLE','VIEW','SEQUENCE','PROCEDURE','FUNCTION','PACKAGE') "
                "AND OWNER NOT IN ('SYS','SYSTEM','DBSNMP','OUTLN','MDSYS','ORDSYS','ORDDATA',"
                "'CTXSYS','XDB','WMSYS','APPQOSSYS','DBSFWUSER','REMOTE_SCHEDULER_AGENT',"
                "'GSMADMIN_INTERNAL','OJVMSYS','LBACSYS','GGSYS') "
                "ORDER BY OWNER"));
        }
        std::vector<DatabasePtr> out;
        out.reserve(owners.size());
        for (const auto& o : owners)
            out.push_back(database(o));
        return out;
    }

    // schema names are case-sensitive; empty = the login user's schema
    DatabasePtr database(const std::string& name) {
        if (!conn_)
            return nullptr;
        const std::string n = name.empty() ? defaultSchema_ : name;
        std::lock_guard lock(cacheMutex_);
        auto& db = cache_[n];
        if (!db)
            db = std::make_shared<OracleDatabase>(ctx_, info_, n);
        return db;
    }

    DatabasePtr openDatabase(const std::string& name) {
        if (!conn_)
            return nullptr;
        return std::make_shared<OracleDatabase>(ctx_, info_, name.empty() ? defaultSchema_ : name);
    }

    // a schema is a user: CREATE USER "name" IDENTIFIED BY "name"
    Status createDatabase(const CreateDatabaseOptions& opts) {
        if (opts.name.empty())
            return {false, "Schema name cannot be empty"};
        std::lock_guard lock(connMutex_);
        if (!conn_)
            return {false, "Not connected to Oracle"};
        const std::string user = quoteIdent(opts.name);
        auto r = runQuery(ctx_, conn_, std::format("CREATE USER {} IDENTIFIED BY {}", user, user),
                          0);
        if (!r.success())
            return {false, r.errorMessage()};
        runQuery(ctx_, conn_, "GRANT CONNECT, RESOURCE TO " + user, 0);
        return {true, ""};
    }

    Status dropDatabase(const std::string& name) {
        {
            std::lock_guard lock(cacheMutex_);
            cache_.erase(name);
        }
        std::lock_guard lock(connMutex_);
        if (!conn_)
            return {false, "Not connected to Oracle"};
        auto r = runQuery(ctx_, conn_, std::format("DROP USER {} CASCADE", quoteIdent(name)), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    ConnectionInfo info_;
    dpiContext* ctx_ = nullptr;
    dpiConn* conn_ = nullptr; // login session: catalog of owners, users DDL
    std::mutex connMutex_;
    std::string defaultSchema_;
    std::unordered_map<std::string, std::shared_ptr<OracleDatabase>> cache_;
    std::mutex cacheMutex_;
};

} // namespace

namespace oracle {

void setClientOptions(const ClientOptions& options) {
    auto& s = ctxState();
    std::lock_guard lock(s.mutex);
    s.options = options;
}

bool needsClientInstall() {
    return getDpiContext(false) == nullptr;
}

void resetContext() {
    auto& s = ctxState();
    std::lock_guard lock(s.mutex);
    s.failed = false;
    s.failedInstalled = false;
    s.autoInstallAttempted = false;
    s.error.clear();
}

} // namespace oracle

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

DatabasePtr OracleConnection::openDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->openDatabase(name);
}

Status OracleConnection::createDatabase(const CreateDatabaseOptions& opts) {
    return static_cast<ConnectionImpl*>(impl_)->createDatabase(opts);
}

Status OracleConnection::dropDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->dropDatabase(name);
}

} // namespace dearsql
