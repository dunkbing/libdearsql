#include "dearsql/backends/cassandra_connection.hpp"

#include <algorithm>
#include <cassandra.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace dearsql {

namespace {

using FuturePtr = std::unique_ptr<CassFuture, decltype(&cass_future_free)>;
using ResultPtr = std::unique_ptr<const CassResult, decltype(&cass_result_free)>;
using StatementPtr = std::unique_ptr<CassStatement, decltype(&cass_statement_free)>;
using IteratorPtr = std::unique_ptr<CassIterator, decltype(&cass_iterator_free)>;

FuturePtr makeFuture(CassFuture* f) {
    return {f, cass_future_free};
}
ResultPtr makeResult(const CassResult* r) {
    return {r, cass_result_free};
}
StatementPtr makeStatement(CassStatement* s) {
    return {s, cass_statement_free};
}
IteratorPtr makeIterator(CassIterator* i) {
    return {i, cass_iterator_free};
}

std::string futureError(CassFuture* future) {
    const char* msg = nullptr;
    size_t len = 0;
    cass_future_error_message(future, &msg, &len);
    return std::string(msg ? msg : "", len);
}

std::string quoteIdent(const std::string& id) {
    std::string out;
    out.reserve(id.size() + 2);
    out += '"';
    for (char c : id) {
        if (c == '"')
            out += '"';
        out += c;
    }
    out += '"';
    return out;
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

std::string cassValueToString(const CassValue* value) {
    if (!value || cass_value_is_null(value))
        return std::string(NULL_SENTINEL);
    const auto type = cass_value_type(value);
    switch (type) {
    case CASS_VALUE_TYPE_VARCHAR:
    case CASS_VALUE_TYPE_ASCII:
    case CASS_VALUE_TYPE_TEXT: {
        const char* s = nullptr;
        size_t n = 0;
        cass_value_get_string(value, &s, &n);
        return std::string(s ? s : "", n);
    }
    case CASS_VALUE_TYPE_INT: {
        cass_int32_t v = 0;
        cass_value_get_int32(value, &v);
        return std::to_string(v);
    }
    case CASS_VALUE_TYPE_BIGINT:
    case CASS_VALUE_TYPE_COUNTER: {
        cass_int64_t v = 0;
        cass_value_get_int64(value, &v);
        return std::to_string(v);
    }
    case CASS_VALUE_TYPE_SMALL_INT: {
        cass_int16_t v = 0;
        cass_value_get_int16(value, &v);
        return std::to_string(v);
    }
    case CASS_VALUE_TYPE_TINY_INT: {
        cass_int8_t v = 0;
        cass_value_get_int8(value, &v);
        return std::to_string(static_cast<int>(v));
    }
    case CASS_VALUE_TYPE_BOOLEAN: {
        cass_bool_t v = cass_false;
        cass_value_get_bool(value, &v);
        return v ? std::string(BOOL_TRUE_SENTINEL) : std::string(BOOL_FALSE_SENTINEL);
    }
    case CASS_VALUE_TYPE_FLOAT: {
        cass_float_t v = 0;
        cass_value_get_float(value, &v);
        return std::to_string(v);
    }
    case CASS_VALUE_TYPE_DOUBLE: {
        cass_double_t v = 0;
        cass_value_get_double(value, &v);
        return std::to_string(v);
    }
    case CASS_VALUE_TYPE_UUID:
    case CASS_VALUE_TYPE_TIMEUUID: {
        CassUuid uuid;
        cass_value_get_uuid(value, &uuid);
        char buf[CASS_UUID_STRING_LENGTH];
        cass_uuid_string(uuid, buf);
        return std::string(buf);
    }
    case CASS_VALUE_TYPE_INET: {
        CassInet inet;
        cass_value_get_inet(value, &inet);
        char buf[CASS_INET_STRING_LENGTH];
        cass_inet_string(inet, buf);
        return std::string(buf);
    }
    case CASS_VALUE_TYPE_TIMESTAMP: {
        cass_int64_t millis = 0;
        cass_value_get_int64(value, &millis);
        const auto seconds = millis / 1000;
        const auto t = static_cast<std::time_t>(seconds);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return std::string(buf);
    }
    default:
        return "<unsupported>";
    }
}

std::string getStringCol(const CassRow* row, const char* name) {
    const CassValue* v = cass_row_get_column_by_name(row, name);
    if (!v || cass_value_is_null(v))
        return "";
    const char* s = nullptr;
    size_t n = 0;
    cass_value_get_string(v, &s, &n);
    return std::string(s ? s : "", n);
}

// run a CQL statement synchronously and return (result, error). result is null on error.
std::pair<ResultPtr, std::string> runCql(CassSession* session, const std::string& cql) {
    if (!session)
        return {ResultPtr{nullptr, cass_result_free}, "Not connected"};
    auto stmt = makeStatement(cass_statement_new(cql.c_str(), 0));
    auto fut = makeFuture(cass_session_execute(session, stmt.get()));
    cass_future_wait(fut.get());
    if (cass_future_error_code(fut.get()) != CASS_OK) {
        return {ResultPtr{nullptr, cass_result_free}, futureError(fut.get())};
    }
    return {makeResult(cass_future_get_result(fut.get())), ""};
}

class CassandraConnImpl;

// A keyspace inside the Cassandra cluster. Holds a shared_ptr back to the
// connection so the session outlives the database handle.
class CassandraDatabase final : public IDatabase {
public:
    CassandraDatabase(std::shared_ptr<CassandraConnImpl> parent, std::string keyspace)
        : parent_(std::move(parent)), name_(std::move(keyspace)) {}

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::CASSANDRA;
    }

    std::vector<Table> tables() override;
    std::vector<Table> views() override;
    Table describeTable(const std::string& tableName) override;

    QueryResult execute(const std::string& sql, int rowLimit) override;

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
                                                       const std::string& orderByClause) override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause) override;

    Status createTable(const Table& table) override;
    Status dropTable(const std::string& tableName) override;

private:
    std::vector<Column> loadColumns(const std::string& tableName);

    std::shared_ptr<CassandraConnImpl> parent_;
    std::string name_;
};

// The shared session-owning impl. Lives as a shared_ptr so CassandraDatabase
// instances can hold a strong reference back to it.
class CassandraConnImpl : public std::enable_shared_from_this<CassandraConnImpl> {
public:
    explicit CassandraConnImpl(ConnectionInfo info) : info_(std::move(info)) {
        if (info_.port == 0 || info_.port == 5432)
            info_.port = 9042;
    }

    ~CassandraConnImpl() {
        freeDriverState();
    }

    Status open() {
        std::lock_guard lock(mu_);
        freeDriverState();

        cluster_ = cass_cluster_new();
        cass_cluster_set_contact_points(cluster_, info_.host.c_str());
        cass_cluster_set_port(cluster_, info_.port);
        if (!info_.username.empty()) {
            cass_cluster_set_credentials(cluster_, info_.username.c_str(),
                                         info_.password.c_str());
        }
        cass_cluster_set_connect_timeout(cluster_, 10000);
        cass_cluster_set_request_timeout(cluster_, 30000);

        if (auto [ok, err] = applySslConfig(); !ok) {
            freeDriverState();
            return {false, err};
        }

        session_ = cass_session_new();
        auto fut = makeFuture(cass_session_connect(session_, cluster_));
        cass_future_wait(fut.get());
        if (cass_future_error_code(fut.get()) != CASS_OK) {
            std::string err = "Cassandra connect failed: " + futureError(fut.get());
            freeDriverState();
            return {false, err};
        }
        open_ = true;
        return {true, ""};
    }

    void close() {
        std::lock_guard lock(mu_);
        cache_.clear();
        freeDriverState();
        open_ = false;
    }

    [[nodiscard]] bool isOpen() const {
        return open_;
    }
    [[nodiscard]] const ConnectionInfo& info() const {
        return info_;
    }

    CassSession* session() {
        return session_;
    }

    // Run a CQL string under the session mutex. Builds a QueryResult.
    QueryResult execute(const std::string& cql, int rowLimit) {
        QueryResult out;
        StatementResult s;
        const auto t0 = std::chrono::high_resolution_clock::now();

        std::lock_guard lock(mu_);
        if (!session_) {
            s.success = false;
            s.errorMessage = "Not connected";
            out.statements.push_back(std::move(s));
            return out;
        }

        auto stmt = makeStatement(cass_statement_new(cql.c_str(), 0));
        cass_statement_set_paging_size(stmt.get(), rowLimit > 0 ? rowLimit : 1000);

        auto fut = makeFuture(cass_session_execute(session_, stmt.get()));
        cass_future_wait(fut.get());
        if (cass_future_error_code(fut.get()) != CASS_OK) {
            s.success = false;
            s.errorMessage = futureError(fut.get());
            out.statements.push_back(std::move(s));
            const auto t1 = std::chrono::high_resolution_clock::now();
            out.executionTimeMs =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            return out;
        }

        auto result = makeResult(cass_future_get_result(fut.get()));
        const size_t cols = cass_result_column_count(result.get());

        if (cols == 0) {
            // DDL / DML — driver reports no rows; report OK.
            s.message = "Query executed successfully";
            out.statements.push_back(std::move(s));
            const auto t1 = std::chrono::high_resolution_clock::now();
            out.executionTimeMs =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
            return out;
        }

        s.columnNames.reserve(cols);
        for (size_t i = 0; i < cols; ++i) {
            const char* name = nullptr;
            size_t nlen = 0;
            cass_result_column_name(result.get(), i, &name, &nlen);
            s.columnNames.emplace_back(name ? name : "", nlen);
        }

        auto it = makeIterator(cass_iterator_from_result(result.get()));
        int taken = 0;
        while (cass_iterator_next(it.get()) && (rowLimit <= 0 || taken < rowLimit)) {
            const CassRow* row = cass_iterator_get_row(it.get());
            std::vector<std::string> r;
            r.reserve(cols);
            for (size_t i = 0; i < cols; ++i)
                r.push_back(cassValueToString(cass_row_get_column(row, i)));
            s.tableData.push_back(std::move(r));
            ++taken;
        }
        s.message = std::format("Returned {} row{}", s.tableData.size(),
                                s.tableData.size() == 1 ? "" : "s");
        out.statements.push_back(std::move(s));
        const auto t1 = std::chrono::high_resolution_clock::now();
        out.executionTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        return out;
    }

    // List keyspaces. Includes system_* keyspaces — callers (e.g. tests) may
    // expect to see them.
    std::vector<std::string> listKeyspaces() {
        std::vector<std::string> out;
        auto r = execute("SELECT keyspace_name FROM system_schema.keyspaces", 10000);
        if (!r.success() || r.empty())
            return out;
        for (const auto& row : r[0].tableData)
            if (!row.empty())
                out.push_back(row[0]);
        return out;
    }

    DatabasePtr database(const std::string& name) {
        std::lock_guard lock(cacheMu_);
        const std::string n = name.empty() ? info_.database : name;
        auto it = cache_.find(n);
        if (it != cache_.end())
            return it->second;
        auto db = std::make_shared<CassandraDatabase>(shared_from_this(), n);
        cache_[n] = db;
        return db;
    }

    std::vector<DatabasePtr> databases() {
        std::vector<DatabasePtr> out;
        for (const auto& ks : listKeyspaces())
            out.push_back(database(ks));
        return out;
    }

    Status createDatabase(const CreateDatabaseOptions& opts) {
        if (!isOpen())
            return {false, "Not connected"};
        const std::string cql =
            "CREATE KEYSPACE IF NOT EXISTS " + quoteIdent(opts.name) +
            " WITH replication = {'class':'SimpleStrategy','replication_factor':1}";
        auto r = execute(cql, 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

    Status dropDatabase(const std::string& name) {
        if (!isOpen())
            return {false, "Not connected"};
        {
            std::lock_guard lock(cacheMu_);
            cache_.erase(name);
        }
        auto r = execute("DROP KEYSPACE IF EXISTS " + quoteIdent(name), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

private:
    void freeDriverState() {
        if (session_) {
            auto fut = makeFuture(cass_session_close(session_));
            cass_future_wait(fut.get());
            cass_session_free(session_);
            session_ = nullptr;
        }
        if (cluster_) {
            cass_cluster_free(cluster_);
            cluster_ = nullptr;
        }
        if (ssl_) {
            cass_ssl_free(ssl_);
            ssl_ = nullptr;
        }
    }

    std::pair<bool, std::string> applySslConfig() {
        if (info_.sslmode == SslMode::Disable)
            return {true, ""};
        ssl_ = cass_ssl_new();

        if (info_.sslmode == SslMode::VerifyCA && !info_.sslCACertPath.empty()) {
            std::ifstream f(info_.sslCACertPath, std::ios::binary);
            if (!f)
                return {false, "Cannot read CA cert: " + info_.sslCACertPath};
            std::ostringstream pem;
            pem << f.rdbuf();
            const std::string s = pem.str();
            if (cass_ssl_add_trusted_cert_n(ssl_, s.data(), s.size()) != CASS_OK)
                return {false, "Failed to add CA cert to SSL context"};
            cass_ssl_set_verify_flags(ssl_, CASS_SSL_VERIFY_PEER_CERT);
        } else {
            // require / no CA: encrypt only, do not verify
            cass_ssl_set_verify_flags(ssl_, CASS_SSL_VERIFY_NONE);
        }

        cass_cluster_set_ssl(cluster_, ssl_);
        return {true, ""};
    }

    ConnectionInfo info_;
    CassCluster* cluster_ = nullptr;
    CassSession* session_ = nullptr;
    CassSsl* ssl_ = nullptr;
    bool open_ = false;
    std::mutex mu_; // guards driver handles + execute()
    std::mutex cacheMu_;
    std::unordered_map<std::string, std::shared_ptr<CassandraDatabase>> cache_;
};

// ---------- CassandraDatabase member impls ----------

QueryResult CassandraDatabase::execute(const std::string& sql, int rowLimit) {
    if (!parent_)
        return {};
    // Cassandra has no per-statement keyspace qualifier for arbitrary CQL.
    // For user queries, prepend USE so unqualified table names resolve in this
    // keyspace. The USE result is dropped from the returned QueryResult.
    if (!name_.empty()) {
        parent_->execute("USE " + quoteIdent(name_), 1);
    }
    return parent_->execute(sql, rowLimit);
}

std::vector<Column> CassandraDatabase::loadColumns(const std::string& tableName) {
    std::vector<Column> cols;
    if (!parent_)
        return cols;

    const std::string cql =
        std::format("SELECT column_name, type, kind, position FROM system_schema.columns "
                    "WHERE keyspace_name = '{}' AND table_name = '{}'",
                    escapeLiteral(name_), escapeLiteral(tableName));
    auto [res, err] = runCql(parent_->session(), cql);
    if (!res)
        return cols;

    // partition_key + clustering form the primary key; sort them first by position.
    struct Pending {
        Column col;
        std::string kind;
        int position = 0;
    };
    std::vector<Pending> pending;

    auto it = makeIterator(cass_iterator_from_result(res.get()));
    while (cass_iterator_next(it.get())) {
        const CassRow* row = cass_iterator_get_row(it.get());
        Pending p;
        p.col.name = getStringCol(row, "column_name");
        p.col.type = getStringCol(row, "type");
        p.kind = getStringCol(row, "kind");
        const CassValue* posVal = cass_row_get_column_by_name(row, "position");
        if (posVal && !cass_value_is_null(posVal)) {
            cass_int32_t v = 0;
            cass_value_get_int32(posVal, &v);
            p.position = v;
        }
        if (p.kind == "partition_key" || p.kind == "clustering") {
            p.col.isPrimaryKey = true;
            p.col.isNotNull = true;
        }
        pending.push_back(std::move(p));
    }

    std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        const int aRank = (a.kind == "partition_key") ? 0 : (a.kind == "clustering") ? 1 : 2;
        const int bRank = (b.kind == "partition_key") ? 0 : (b.kind == "clustering") ? 1 : 2;
        if (aRank != bRank)
            return aRank < bRank;
        if (aRank < 2)
            return a.position < b.position;
        return a.col.name < b.col.name;
    });

    cols.reserve(pending.size());
    for (auto& p : pending)
        cols.push_back(std::move(p.col));
    return cols;
}

std::vector<Table> CassandraDatabase::tables() {
    std::vector<Table> result;
    if (!parent_)
        return result;
    const std::string cql =
        std::format("SELECT table_name FROM system_schema.tables WHERE keyspace_name = '{}'",
                    escapeLiteral(name_));
    auto [res, err] = runCql(parent_->session(), cql);
    if (!res)
        return result;

    auto it = makeIterator(cass_iterator_from_result(res.get()));
    while (cass_iterator_next(it.get())) {
        const CassRow* row = cass_iterator_get_row(it.get());
        Table t;
        t.name = getStringCol(row, "table_name");
        t.schema = name_;
        t.fullName = name_ + "." + t.name;
        t.columns = loadColumns(t.name);
        result.push_back(std::move(t));
    }
    return result;
}

std::vector<Table> CassandraDatabase::views() {
    std::vector<Table> result;
    if (!parent_)
        return result;
    // Cassandra has no plain views — only materialized views in system_schema.views.
    const std::string cql = std::format(
        "SELECT view_name, base_table_name FROM system_schema.views WHERE keyspace_name = '{}'",
        escapeLiteral(name_));
    auto [res, err] = runCql(parent_->session(), cql);
    if (!res)
        return result;

    auto it = makeIterator(cass_iterator_from_result(res.get()));
    while (cass_iterator_next(it.get())) {
        const CassRow* row = cass_iterator_get_row(it.get());
        Table v;
        v.name = getStringCol(row, "view_name");
        v.schema = name_;
        v.fullName = name_ + "." + v.name;
        v.definition = "Materialized view of " + getStringCol(row, "base_table_name");
        v.columns = loadColumns(v.name);
        result.push_back(std::move(v));
    }
    return result;
}

Table CassandraDatabase::describeTable(const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.schema = name_;
    t.fullName = name_ + "." + tableName;
    t.columns = loadColumns(tableName);
    return t;
}

std::vector<std::vector<std::string>>
CassandraDatabase::getTableData(const Table& table, int limit, int offset,
                                const std::string& whereClause,
                                const std::string& orderByClause) {
    // CQL lacks OFFSET; sidebar paging requests offset=0 first. Ignore offset.
    (void)offset;
    std::vector<std::vector<std::string>> rows;
    if (!parent_)
        return rows;

    std::string cql = "SELECT * FROM " + quoteIdent(name_) + "." + quoteIdent(table.name);
    if (!whereClause.empty())
        cql += " WHERE " + whereClause + " ALLOW FILTERING";
    if (!orderByClause.empty())
        cql += " ORDER BY " + orderByClause;
    cql += " LIMIT " + std::to_string(limit > 0 ? limit : 1000);

    QueryResult r = parent_->execute(cql, limit);
    if (r.empty())
        return rows;
    return r[0].tableData;
}

std::vector<std::string> CassandraDatabase::getColumnNames(const Table& table) {
    std::vector<std::string> names;
    if (!table.columns.empty()) {
        names.reserve(table.columns.size());
        for (const auto& c : table.columns)
            names.push_back(c.name);
        return names;
    }
    for (const auto& c : loadColumns(table.name))
        names.push_back(c.name);
    return names;
}

int CassandraDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    if (!parent_)
        return 0;
    std::string cql = "SELECT COUNT(*) FROM " + quoteIdent(name_) + "." + quoteIdent(table.name);
    if (!whereClause.empty())
        cql += " WHERE " + whereClause + " ALLOW FILTERING";

    QueryResult r = parent_->execute(cql, 1);
    if (!r.success() || r.empty() || r[0].tableData.empty() || r[0].tableData[0].empty())
        return 0;
    try {
        return std::stoi(r[0].tableData[0][0]);
    } catch (...) {
        return 0;
    }
}

Status CassandraDatabase::createTable(const Table& table) {
    if (!parent_)
        return {false, "Not connected"};
    std::string sql =
        "CREATE TABLE " + quoteIdent(name_) + "." + quoteIdent(table.name) + " (";
    bool first = true;
    std::vector<std::string> pkCols;
    for (const auto& c : table.columns) {
        if (!first)
            sql += ", ";
        first = false;
        sql += quoteIdent(c.name) + " " + (c.type.empty() ? std::string("text") : c.type);
        if (c.isPrimaryKey)
            pkCols.push_back(c.name);
    }
    if (!pkCols.empty()) {
        sql += ", PRIMARY KEY (";
        for (size_t i = 0; i < pkCols.size(); ++i) {
            if (i)
                sql += ", ";
            sql += quoteIdent(pkCols[i]);
        }
        sql += ")";
    }
    sql += ")";

    auto r = parent_->execute(sql, 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status CassandraDatabase::dropTable(const std::string& tableName) {
    if (!parent_)
        return {false, "Not connected"};
    auto r = parent_->execute(
        "DROP TABLE " + quoteIdent(name_) + "." + quoteIdent(tableName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

// Holder for the shared_ptr stored behind void* impl_.
struct CassandraImplHolder {
    std::shared_ptr<CassandraConnImpl> impl;
};

} // namespace

// ---------- CassandraConnection ----------

CassandraConnection::CassandraConnection(const ConnectionInfo& info) : info_(info) {
    if (info_.port == 0 || info_.port == 5432)
        info_.port = 9042;
    auto* h = new CassandraImplHolder;
    h->impl = std::make_shared<CassandraConnImpl>(info_);
    impl_ = h;
}

CassandraConnection::~CassandraConnection() {
    if (impl_) {
        auto* h = static_cast<CassandraImplHolder*>(impl_);
        if (h->impl)
            h->impl->close();
        delete h;
        impl_ = nullptr;
    }
}

Status CassandraConnection::open() {
    auto* h = static_cast<CassandraImplHolder*>(impl_);
    return h->impl->open();
}

void CassandraConnection::close() {
    auto* h = static_cast<CassandraImplHolder*>(impl_);
    if (h && h->impl)
        h->impl->close();
}

bool CassandraConnection::isOpen() const {
    auto* h = static_cast<const CassandraImplHolder*>(impl_);
    return h && h->impl && h->impl->isOpen();
}

std::vector<DatabasePtr> CassandraConnection::databases() {
    auto* h = static_cast<CassandraImplHolder*>(impl_);
    if (!h || !h->impl)
        return {};
    return h->impl->databases();
}

DatabasePtr CassandraConnection::database(const std::string& name) {
    auto* h = static_cast<CassandraImplHolder*>(impl_);
    if (!h || !h->impl)
        return nullptr;
    return h->impl->database(name);
}

Status CassandraConnection::createDatabase(const CreateDatabaseOptions& opts) {
    auto* h = static_cast<CassandraImplHolder*>(impl_);
    if (!h || !h->impl)
        return {false, "Not connected"};
    return h->impl->createDatabase(opts);
}

Status CassandraConnection::dropDatabase(const std::string& name) {
    auto* h = static_cast<CassandraImplHolder*>(impl_);
    if (!h || !h->impl)
        return {false, "Not connected"};
    return h->impl->dropDatabase(name);
}

} // namespace dearsql
