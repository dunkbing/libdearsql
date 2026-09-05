#include "dearsql/backends/mysql_connection.hpp"
#include "dearsql/ddl_utils.hpp"

#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <mysql/mysql.h>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace dearsql {

namespace {

struct MysqlResDeleter {
    void operator()(MYSQL_RES* r) const {
        if (r)
            mysql_free_result(r);
    }
};
using MysqlResPtr = std::unique_ptr<MYSQL_RES, MysqlResDeleter>;

void drainExtraResults(MYSQL* conn) {
    while (mysql_next_result(conn) == 0) {
        if (MYSQL_RES* extra = mysql_store_result(conn))
            mysql_free_result(extra);
    }
}

std::string quoteIdent(const std::string& s) {
    std::string out = "`";
    for (char c : s) {
        if (c == '`')
            out += "``";
        else
            out += c;
    }
    out += "`";
    return out;
}

std::string escapeLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\'')
            out += "''";
        else if (c == '\\')
            out += "\\\\";
        else
            out += c;
    }
    return out;
}

bool shouldApplySslCA(const ConnectionInfo& info) {
    if (info.sslCACertPath.empty())
        return false;
    switch (info.sslmode) {
    case SslMode::VerifyCA:
    case SslMode::VerifyFull:
    case SslMode::VerifyIdentity:
        return true;
    default:
        return false;
    }
}

// Open a fresh MYSQL* connected to the requested database. Caller owns the
// returned handle and must mysql_close() it. Throws on failure.
MYSQL* openMysql(const ConnectionInfo& info, const std::string& dbName) {
    MYSQL* conn = mysql_init(nullptr);
    if (!conn)
        throw std::runtime_error("mysql_init failed");

    constexpr unsigned int connectTimeoutSeconds = 5;
    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connectTimeoutSeconds);

    constexpr unsigned int tcpProtocol = MYSQL_PROTOCOL_TCP;
    mysql_options(conn, MYSQL_OPT_PROTOCOL, &tcpProtocol);

    if (shouldApplySslCA(info))
        mysql_options(conn, MYSQL_OPT_SSL_CA, info.sslCACertPath.c_str());

    {
        my_bool enforce = 0;
        my_bool verify = 0;
        switch (info.sslmode) {
        case SslMode::Disable:
            enforce = 0;
            verify = 0;
            break;
        case SslMode::Require:
            enforce = 1;
            verify = 0;
            break;
        case SslMode::VerifyCA:
        case SslMode::VerifyFull:
        case SslMode::VerifyIdentity:
            enforce = 1;
            verify = 1;
            break;
        default:
            enforce = 0;
            verify = 0;
            break;
        }
        mysql_options(conn, MYSQL_OPT_SSL_ENFORCE, &enforce);
        mysql_options(conn, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
    }

    const char* db = dbName.empty() ? nullptr : dbName.c_str();
    if (!mysql_real_connect(conn, info.host.c_str(), info.username.c_str(), info.password.c_str(),
                            db, info.port, nullptr, CLIENT_MULTI_STATEMENTS)) {
        std::string err = mysql_error(conn);
        mysql_close(conn);
        throw std::runtime_error("MySQL connection failed: " + err);
    }
    mysql_set_character_set(conn, "utf8mb4");
    return conn;
}

using Clock = std::chrono::high_resolution_clock;
double toMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// downloadMs/parseMs accumulate the store_result and row-copy time
StatementResult extractMysqlResult(MYSQL* conn, int rowLimit, double* downloadMs = nullptr,
                                   double* parseMs = nullptr) {
    StatementResult result;

    const auto tDownload = Clock::now();
    MYSQL_RES* rawRes = mysql_store_result(conn);
    const auto tParse = Clock::now();
    if (downloadMs)
        *downloadMs += toMs(tParse - tDownload);

    if (rawRes) {
        MysqlResPtr res(rawRes);
        unsigned int nFields = mysql_num_fields(res.get());
        MYSQL_FIELD* fields = mysql_fetch_fields(res.get());

        std::vector<bool> isBoolCol(nFields, false);
        for (unsigned int i = 0; i < nFields; i++) {
            result.columnNames.emplace_back(fields[i].name);
            isBoolCol[i] = (fields[i].type == MYSQL_TYPE_TINY && fields[i].length == 1);
        }

        int rowCount = 0;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get())) != nullptr && rowCount < rowLimit) {
            unsigned long* lengths = mysql_fetch_lengths(res.get());
            std::vector<std::string> rowData;
            rowData.reserve(nFields);
            for (unsigned int i = 0; i < nFields; i++) {
                if (row[i] == nullptr) {
                    rowData.emplace_back(NULL_SENTINEL);
                } else if (isBoolCol[i]) {
                    rowData.emplace_back(row[i][0] == '1' ? BOOL_TRUE_SENTINEL
                                                          : BOOL_FALSE_SENTINEL);
                } else {
                    rowData.emplace_back(row[i], lengths[i]);
                }
            }
            result.tableData.push_back(std::move(rowData));
            rowCount++;
        }

        result.message = std::format("Returned {} row{}", result.tableData.size(),
                                     result.tableData.size() == 1 ? "" : "s");
        my_ulonglong totalRows = mysql_num_rows(res.get());
        if (rowLimit > 0 && static_cast<my_ulonglong>(rowLimit) <= totalRows &&
            static_cast<int>(totalRows) > rowLimit) {
            result.message += std::format(" (limited to {})", rowLimit);
        }
    } else {
        if (mysql_field_count(conn) == 0) {
            my_ulonglong affected = mysql_affected_rows(conn);
            if (affected != static_cast<my_ulonglong>(-1)) {
                result.affectedRows = static_cast<int>(affected);
                result.message = std::format("{} row(s) affected", affected);
            } else {
                result.message = "Query executed successfully";
            }
        } else {
            result.success = false;
            result.errorMessage = mysql_error(conn);
        }
    }

    if (parseMs)
        *parseMs += toMs(Clock::now() - tParse);
    return result;
}

} // namespace

// ---------- MySQLDatabase ----------

MySQLDatabase::MySQLDatabase(ConnectionInfo info, std::string dbName)
    : info_(std::move(info)), name_(std::move(dbName)) {}

MySQLDatabase::~MySQLDatabase() {
    if (conn_)
        mysql_close(conn_);
}

void MySQLDatabase::ensureConn() {
    if (conn_)
        return;
    conn_ = openMysql(info_, name_);
}

Status MySQLDatabase::open() {
    try {
        ensureConn();
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

bool MySQLDatabase::ping() {
    if (!conn_)
        return false;
    std::lock_guard lock(mu_);
    return mysql_ping(conn_) == 0;
}

void MySQLDatabase::cancel() {
    // a busy MYSQL* cannot issue anything itself, so kill from a throwaway connection
    if (!conn_)
        return;
    const auto threadId = mysql_thread_id(conn_);
    try {
        MYSQL* killer = openMysql(info_, "");
        const std::string sql = std::format("KILL QUERY {}", threadId);
        mysql_query(killer, sql.c_str());
        mysql_close(killer);
    } catch (const std::exception&) {
    }
}

QueryResult MySQLDatabase::execute(const std::string& sql, int rowLimit) {
    QueryResult result;
    const auto startTime = Clock::now();

    try {
        ensureConn();
    } catch (const std::exception& e) {
        StatementResult r;
        r.success = false;
        r.errorMessage = e.what();
        result.statements.push_back(std::move(r));
        return result;
    }

    std::lock_guard lock(mu_);
    mysql_ping(conn_); // round trip ≈ network latency
    const auto tPing = Clock::now();

    // real_query takes a length: sql may be a megabyte-sized batch
    if (mysql_real_query(conn_, sql.c_str(), sql.size()) != 0) {
        StatementResult r;
        r.success = false;
        r.errorMessage = mysql_error(conn_);
        result.statements.push_back(std::move(r));
        result.executionTimeMs = toMs(Clock::now() - startTime);
        return result;
    }
    const auto tExec = Clock::now();

    double downloadMs = 0.0;
    double parseMs = 0.0;
    // mysql_next_result returns 0 for another result, -1 when there are no more,
    // and >0 for an error. Only real_query reports a failure in the first
    // statement of a batch; a later one arrives here, so stopping on >0 without
    // recording it would report a partial batch as applied.
    int next = 0;
    do {
        auto r = extractMysqlResult(conn_, rowLimit, &downloadMs, &parseMs);
        if (r.success || !r.errorMessage.empty())
            result.statements.push_back(std::move(r));
        next = mysql_next_result(conn_);
    } while (next == 0);
    if (next > 0) {
        StatementResult r;
        r.success = false;
        r.errorMessage = mysql_error(conn_);
        result.statements.push_back(std::move(r));
    }

    // coarse client-side split; execution includes one-way latency, rows
    // arrive during mysql_store_result (data download)
    result.phaseTimings = {{"network latency", toMs(tPing - startTime)},
                           {"execution", toMs(tExec - tPing)},
                           {"data download", downloadMs},
                           {"data parse", parseMs}};
    result.executionTimeMs = toMs(Clock::now() - startTime);
    return result;
}

std::vector<Table> MySQLDatabase::tables() {
    std::vector<Table> result;
    try {
        ensureConn();
    } catch (...) {
        return result;
    }

    std::lock_guard lock(mu_);

    std::vector<std::string> tableNames;
    if (mysql_query(conn_, "SHOW FULL TABLES WHERE Table_type = 'BASE TABLE'") == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                if (row[0])
                    tableNames.emplace_back(row[0]);
            }
        }
    }
    drainExtraResults(conn_);

    for (const auto& tableName : tableNames) {
        Table table;
        table.name = tableName;
        table.schema = name_;
        table.fullName = info_.name + "." + name_ + "." + tableName;

        const std::string columnsQuery = std::format("DESCRIBE {}", quoteIdent(tableName));
        if (mysql_query(conn_, columnsQuery.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn_));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    Column col;
                    col.name = row[0] ? row[0] : "";
                    col.type = row[1] ? row[1] : "";
                    col.isNotNull = row[2] && std::string(row[2]) == "NO";
                    col.isPrimaryKey = row[3] && std::string(row[3]) == "PRI";
                    if (row[4])
                        col.defaultValue = row[4];
                    col.isAutoIncrement = row[5] && std::string(row[5]).find("auto_increment") !=
                                                        std::string::npos;
                    table.columns.push_back(std::move(col));
                }
            }
        }
        drainExtraResults(conn_);

        const std::string fkQuery = std::format(
            "SELECT COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME, "
            "CONSTRAINT_NAME "
            "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
            "WHERE TABLE_SCHEMA = '{}' AND TABLE_NAME = '{}' "
            "AND REFERENCED_TABLE_NAME IS NOT NULL",
            escapeLiteral(name_), escapeLiteral(tableName));
        if (mysql_query(conn_, fkQuery.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn_));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    ForeignKey fk;
                    fk.sourceColumn = row[0] ? row[0] : "";
                    fk.targetTable = row[1] ? row[1] : "";
                    fk.targetColumn = row[2] ? row[2] : "";
                    fk.name = row[3] ? row[3] : "";
                    table.foreignKeys.push_back(std::move(fk));
                }
            }
        }
        drainExtraResults(conn_);

        buildForeignKeyLookup(table);
        result.push_back(std::move(table));
    }

    populateIncomingForeignKeys(result);
    return result;
}

std::vector<Table> MySQLDatabase::views() {
    std::vector<Table> result;
    try {
        ensureConn();
    } catch (...) {
        return result;
    }

    std::lock_guard lock(mu_);

    std::vector<std::string> viewNames;
    if (mysql_query(conn_, "SHOW FULL TABLES WHERE Table_type = 'VIEW'") == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                if (row[0])
                    viewNames.emplace_back(row[0]);
            }
        }
    }
    drainExtraResults(conn_);

    for (const auto& viewName : viewNames) {
        Table view;
        view.name = viewName;
        view.schema = name_;
        view.fullName = info_.name + "." + name_ + "." + viewName;

        const std::string columnsQuery = std::format("DESCRIBE {}", quoteIdent(viewName));
        if (mysql_query(conn_, columnsQuery.c_str()) == 0) {
            MysqlResPtr res(mysql_store_result(conn_));
            if (res) {
                MYSQL_ROW row;
                while ((row = mysql_fetch_row(res.get())) != nullptr) {
                    Column col;
                    col.name = row[0] ? row[0] : "";
                    col.type = row[1] ? row[1] : "";
                    col.isNotNull = row[2] && std::string(row[2]) == "NO";
                    view.columns.push_back(std::move(col));
                }
            }
        }
        drainExtraResults(conn_);

        result.push_back(std::move(view));
    }

    return result;
}

std::vector<Routine> MySQLDatabase::routines() {
    std::vector<Routine> result;
    try {
        ensureConn();
    } catch (...) {
        return result;
    }

    std::lock_guard lock(mu_);

    const std::string query = std::format(
        "SELECT ROUTINE_NAME, "
        "CONCAT(ROUTINE_NAME, '(', IFNULL("
        "(SELECT GROUP_CONCAT(PARAMETER_NAME, ' ', DATA_TYPE ORDER BY ORDINAL_POSITION) "
        "FROM information_schema.PARAMETERS p "
        "WHERE p.SPECIFIC_SCHEMA = r.ROUTINE_SCHEMA "
        "AND p.SPECIFIC_NAME = r.ROUTINE_NAME "
        "AND p.PARAMETER_MODE IS NOT NULL), ''), ')') AS signature, "
        "ROUTINE_TYPE, "
        "DTD_IDENTIFIER "
        "FROM information_schema.ROUTINES r "
        "WHERE ROUTINE_SCHEMA = '{}' "
        "ORDER BY ROUTINE_TYPE, ROUTINE_NAME",
        escapeLiteral(name_));

    if (mysql_query(conn_, query.c_str()) == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                Routine routine;
                routine.name = row[0] ? row[0] : "";
                routine.signature = row[1] ? row[1] : "";
                std::string routineType = row[2] ? row[2] : "";
                routine.kind = (routineType == "PROCEDURE") ? RoutineKind::Procedure
                                                            : RoutineKind::Function;
                routine.returnType = row[3] ? row[3] : "";
                result.push_back(std::move(routine));
            }
        }
    }
    drainExtraResults(conn_);

    return result;
}

Table MySQLDatabase::describeTable(const std::string& tableName) {
    Table refreshedTable;
    refreshedTable.name = tableName;
    refreshedTable.schema = name_;
    refreshedTable.fullName = info_.name + "." + name_ + "." + tableName;

    try {
        ensureConn();
    } catch (...) {
        return refreshedTable;
    }

    std::lock_guard lock(mu_);

    const std::string columnsQuery = std::format("DESCRIBE {}", quoteIdent(tableName));
    if (mysql_query(conn_, columnsQuery.c_str()) == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                Column col;
                col.name = row[0] ? row[0] : "";
                col.type = row[1] ? row[1] : "";
                col.isNotNull = row[2] && std::string(row[2]) == "NO";
                col.isPrimaryKey = row[3] && std::string(row[3]) == "PRI";
                if (row[4])
                    col.defaultValue = row[4];
                col.isAutoIncrement = row[5] && std::string(row[5]).find("auto_increment") !=
                                                    std::string::npos;
                refreshedTable.columns.push_back(std::move(col));
            }
        }
    }
    drainExtraResults(conn_);

    // unique columns via SHOW INDEX (Non_unique=0 and single-column index)
    const std::string uniqQuery =
        std::format("SHOW INDEX FROM {} WHERE Non_unique = 0", quoteIdent(tableName));
    std::map<std::string, std::vector<std::string>> uniqueIndexCols;
    if (mysql_query(conn_, uniqQuery.c_str()) == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                std::string indexName = row[2] ? row[2] : "";
                std::string columnName = row[4] ? row[4] : "";
                if (!indexName.empty() && !columnName.empty())
                    uniqueIndexCols[indexName].push_back(columnName);
            }
        }
    }
    drainExtraResults(conn_);
    for (auto& col : refreshedTable.columns) {
        for (const auto& [idxName, cols] : uniqueIndexCols) {
            if (cols.size() == 1 && cols[0] == col.name && idxName != "PRIMARY") {
                col.isUnique = true;
                break;
            }
        }
    }

    const std::string indexQuery =
        std::format("SHOW INDEX FROM {} WHERE Key_name != 'PRIMARY'", quoteIdent(tableName));
    if (mysql_query(conn_, indexQuery.c_str()) == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            std::map<std::string, Index> indexMap;
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                std::string indexName = row[2] ? row[2] : "";
                std::string columnName = row[4] ? row[4] : "";
                int nonUnique = row[1] ? std::atoi(row[1]) : 1;
                if (!indexMap.contains(indexName)) {
                    Index idx;
                    idx.name = indexName;
                    idx.isUnique = (nonUnique == 0);
                    indexMap[indexName] = idx;
                }
                indexMap[indexName].columns.push_back(columnName);
            }
            for (auto& [_, idx] : indexMap)
                refreshedTable.indexes.push_back(std::move(idx));
        }
    }
    drainExtraResults(conn_);

    const std::string fkQuery = std::format(
        "SELECT COLUMN_NAME, REFERENCED_TABLE_NAME, REFERENCED_COLUMN_NAME, CONSTRAINT_NAME "
        "FROM INFORMATION_SCHEMA.KEY_COLUMN_USAGE "
        "WHERE TABLE_SCHEMA = '{}' AND TABLE_NAME = '{}' AND REFERENCED_TABLE_NAME IS NOT NULL",
        escapeLiteral(name_), escapeLiteral(tableName));
    if (mysql_query(conn_, fkQuery.c_str()) == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                ForeignKey fk;
                fk.sourceColumn = row[0] ? row[0] : "";
                fk.targetTable = row[1] ? row[1] : "";
                fk.targetColumn = row[2] ? row[2] : "";
                fk.name = row[3] ? row[3] : "";
                refreshedTable.foreignKeys.push_back(std::move(fk));
            }
        }
    }
    drainExtraResults(conn_);

    buildForeignKeyLookup(refreshedTable);
    return refreshedTable;
}

std::vector<std::vector<std::string>>
MySQLDatabase::getTableData(const Table& table, int limit, int offset,
                            const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;
    try {
        ensureConn();
    } catch (...) {
        return result;
    }

    std::string sql = "SELECT * FROM " + quoteIdent(table.name);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;
    if (!orderByClause.empty())
        sql += " ORDER BY " + orderByClause;
    sql += std::format(" LIMIT {} OFFSET {}", limit, offset);

    std::lock_guard lock(mu_);
    if (mysql_query(conn_, sql.c_str()) != 0)
        return result;

    MysqlResPtr res(mysql_store_result(conn_));
    if (!res) {
        drainExtraResults(conn_);
        return result;
    }

    unsigned int nFields = mysql_num_fields(res.get());
    MYSQL_FIELD* fields = mysql_fetch_fields(res.get());
    std::vector<bool> isBoolCol(nFields, false);
    for (unsigned int i = 0; i < nFields; i++) {
        isBoolCol[i] = (fields[i].type == MYSQL_TYPE_TINY && fields[i].length == 1);
    }

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res.get())) != nullptr) {
        unsigned long* lengths = mysql_fetch_lengths(res.get());
        std::vector<std::string> rowData;
        rowData.reserve(nFields);
        for (unsigned int i = 0; i < nFields; i++) {
            if (row[i] == nullptr) {
                rowData.emplace_back(NULL_SENTINEL);
            } else if (isBoolCol[i]) {
                rowData.emplace_back(row[i][0] == '1' ? BOOL_TRUE_SENTINEL : BOOL_FALSE_SENTINEL);
            } else {
                rowData.emplace_back(row[i], lengths[i]);
            }
        }
        result.push_back(std::move(rowData));
    }
    res.reset();
    drainExtraResults(conn_);
    return result;
}

std::vector<std::string> MySQLDatabase::getColumnNames(const Table& table) {
    std::vector<std::string> names;
    try {
        ensureConn();
    } catch (...) {
        return names;
    }

    std::lock_guard lock(mu_);
    const std::string query = std::format("DESCRIBE {}", quoteIdent(table.name));
    if (mysql_query(conn_, query.c_str()) == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get())) != nullptr) {
                if (row[0])
                    names.emplace_back(row[0]);
            }
        }
    }
    drainExtraResults(conn_);
    return names;
}

int MySQLDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    int count = 0;
    try {
        ensureConn();
    } catch (...) {
        return 0;
    }

    std::string sql = "SELECT COUNT(*) FROM " + quoteIdent(table.name);
    if (!whereClause.empty())
        sql += " WHERE " + whereClause;

    std::lock_guard lock(mu_);
    if (mysql_query(conn_, sql.c_str()) == 0) {
        MysqlResPtr res(mysql_store_result(conn_));
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res.get());
            if (row && row[0])
                count = std::atoi(row[0]);
        }
    }
    drainExtraResults(conn_);
    return count;
}

Status MySQLDatabase::createTable(const Table& table) {
    std::string sql = "CREATE TABLE " + quoteIdent(table.name) + " (";
    bool first = true;
    for (const auto& c : table.columns) {
        if (!first)
            sql += ", ";
        first = false;
        sql += quoteIdent(c.name) + " " + (c.type.empty() ? std::string("TEXT") : c.type);
        if (c.isAutoIncrement)
            sql += " AUTO_INCREMENT";
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
        sql += ", FOREIGN KEY (" + quoteIdent(fk.sourceColumn) + ") REFERENCES " +
               quoteIdent(fk.targetTable) + "(" + quoteIdent(fk.targetColumn) + ")";
        if (!fk.onUpdate.empty())
            sql += " ON UPDATE " + fk.onUpdate;
        if (!fk.onDelete.empty())
            sql += " ON DELETE " + fk.onDelete;
    }
    sql += ")";

    auto r = execute(sql, 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status MySQLDatabase::renameTable(const std::string& oldName, const std::string& newName) {
    auto r = execute(std::format("RENAME TABLE {} TO {}", quoteIdent(oldName), quoteIdent(newName)),
                     0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status MySQLDatabase::dropTable(const std::string& tableName) {
    auto r = execute("DROP TABLE " + quoteIdent(tableName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status MySQLDatabase::truncateTable(const std::string& tableName) {
    auto r = execute("TRUNCATE TABLE " + quoteIdent(tableName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status MySQLDatabase::dropColumn(const std::string& tableName, const std::string& columnName) {
    auto r = execute(std::format("ALTER TABLE {} DROP COLUMN {}", quoteIdent(tableName),
                                 quoteIdent(columnName)),
                     0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

Status MySQLDatabase::dropView(const std::string& viewName, bool /*isMaterialized*/) {
    auto r = execute("DROP VIEW " + quoteIdent(viewName), 0);
    return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
}

namespace {

// Top-level connection. Caches one MySQLDatabase per database name.
class ConnectionImpl {
public:
    // an empty database is kept empty: no default schema, so users without
    // grants on `mysql` can still connect
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {}

    Status open() {
        try {
            auto db = std::dynamic_pointer_cast<MySQLDatabase>(database(info_.database));
            if (!db)
                return {false, "could not create default database handle"};
            auto st = db->open();
            open_ = st.first;
            return st;
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
        auto def = std::dynamic_pointer_cast<MySQLDatabase>(database(info_.database));
        if (!def)
            return {};
        auto r = def->execute("SHOW DATABASES", 10000);
        std::vector<DatabasePtr> out;
        if (!r.success() || r.empty())
            return out;
        for (const auto& row : r[0].tableData) {
            if (row.empty())
                continue;
            const std::string& n = row[0];
            if (n == "information_schema" || n == "performance_schema" || n == "mysql" ||
                n == "sys")
                continue;
            out.push_back(database(n));
        }
        return out;
    }

    DatabasePtr database(const std::string& name) {
        const std::string n = name.empty() ? info_.database : name;
        auto it = cache_.find(n);
        if (it != cache_.end())
            return it->second;
        auto db = std::make_shared<MySQLDatabase>(info_, n);
        cache_[n] = db;
        return db;
    }

    DatabasePtr openDatabase(const std::string& name) {
        return std::make_shared<MySQLDatabase>(info_, name.empty() ? info_.database : name);
    }

    Status createDatabase(const CreateDatabaseOptions& opts) {
        auto def = std::dynamic_pointer_cast<MySQLDatabase>(database(info_.database));
        if (!def)
            return {false, "no default connection"};
        if (opts.name.empty())
            return {false, "Database name cannot be empty"};
        if (!opts.charset.empty() && !ddl_utils::isSafeSqlToken(opts.charset))
            return {false, "Invalid charset value"};
        if (!opts.collation.empty() && !ddl_utils::isSafeSqlToken(opts.collation))
            return {false, "Invalid collation value"};
        std::string sql = "CREATE DATABASE " + quoteIdent(opts.name);
        if (!opts.charset.empty())
            sql += " CHARACTER SET " + opts.charset;
        if (!opts.collation.empty())
            sql += " COLLATE " + opts.collation;
        auto r = def->execute(sql, 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

    Status dropDatabase(const std::string& name) {
        // dropping the connected database goes through a temp connection with
        // no default schema, so the open handle is not pulled from under us
        const bool isDroppingConnectedDb = (name == info_.database);
        cache_.erase(name);

        if (isDroppingConnectedDb) {
            MYSQL* tempConn = nullptr;
            try {
                tempConn = openMysql(info_, "");
            } catch (const std::exception& e) {
                return {false, std::format("Failed to connect to system database: {}", e.what())};
            }
            const std::string sql = "DROP DATABASE " + quoteIdent(name);
            if (mysql_query(tempConn, sql.c_str()) != 0) {
                std::string err = mysql_error(tempConn);
                mysql_close(tempConn);
                return {false, err};
            }
            mysql_close(tempConn);
            info_.database = ""; // no default schema from here on
            return {true, ""};
        }

        auto def = std::dynamic_pointer_cast<MySQLDatabase>(database(info_.database));
        if (!def)
            return {false, "no default connection"};
        auto r = def->execute("DROP DATABASE " + quoteIdent(name), 0);
        return r.success() ? Status{true, ""} : Status{false, r.errorMessage()};
    }

    Status renameDatabase(const std::string& /*oldName*/, const std::string& /*newName*/) {
        return {false, "MySQL does not support direct database renaming. "
                       "You need to create a new database, copy all data, and drop the old one."};
    }

private:
    ConnectionInfo info_;
    bool open_ = false;
    std::unordered_map<std::string, std::shared_ptr<MySQLDatabase>> cache_;
};
} // namespace

MySQLConnection::MySQLConnection(const ConnectionInfo& info) : info_(info) {
    impl_ = new ConnectionImpl(info);
}
MySQLConnection::~MySQLConnection() {
    delete static_cast<ConnectionImpl*>(impl_);
}
Status MySQLConnection::open() {
    return static_cast<ConnectionImpl*>(impl_)->open();
}
void MySQLConnection::close() {
    static_cast<ConnectionImpl*>(impl_)->close();
}
bool MySQLConnection::isOpen() const {
    return static_cast<const ConnectionImpl*>(impl_)->isOpen();
}
std::vector<DatabasePtr> MySQLConnection::databases() {
    return static_cast<ConnectionImpl*>(impl_)->databases();
}
DatabasePtr MySQLConnection::database(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->database(name);
}
DatabasePtr MySQLConnection::openDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->openDatabase(name);
}
Status MySQLConnection::createDatabase(const CreateDatabaseOptions& opts) {
    return static_cast<ConnectionImpl*>(impl_)->createDatabase(opts);
}
Status MySQLConnection::dropDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->dropDatabase(name);
}
Status MySQLConnection::renameDatabase(const std::string& oldName, const std::string& newName) {
    return static_cast<ConnectionImpl*>(impl_)->renameDatabase(oldName, newName);
}

} // namespace dearsql
