#include "dearsql/factory.hpp"

#include "dearsql/backends/sqlite_connection.hpp"
#ifdef DEARSQL_HAS_CASSANDRA
#include "dearsql/backends/cassandra_connection.hpp"
#endif
#ifdef DEARSQL_HAS_DUCKDB
#include "dearsql/backends/duckdb_connection.hpp"
#endif
#ifdef DEARSQL_HAS_MONGODB
#include "dearsql/backends/mongodb_connection.hpp"
#endif
#ifdef DEARSQL_HAS_MSSQL
#include "dearsql/backends/mssql_connection.hpp"
#endif
#ifdef DEARSQL_HAS_MYSQL
#include "dearsql/backends/mysql_connection.hpp"
#endif
#ifdef DEARSQL_HAS_ORACLE
#include "dearsql/backends/oracle_connection.hpp"
#endif
#ifdef DEARSQL_HAS_POSTGRES
#include "dearsql/backends/postgres_connection.hpp"
#endif
#ifdef DEARSQL_HAS_REDIS
#include "dearsql/backends/redis_connection.hpp"
#endif

namespace dearsql {

// a backend left out of this build (its driver was not found) gives null
ConnectionPtr makeConnection(const ConnectionInfo& info) {
    switch (info.type) {
    case DatabaseType::SQLITE:
        return std::make_shared<SQLiteConnection>(info);
#ifdef DEARSQL_HAS_POSTGRES
    case DatabaseType::POSTGRESQL:
    case DatabaseType::REDSHIFT:
    case DatabaseType::COCKROACHDB:
        return std::make_shared<PostgresConnection>(info);
#endif
#ifdef DEARSQL_HAS_MYSQL
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
    case DatabaseType::TIDB:
        return std::make_shared<MySQLConnection>(info);
#endif
#ifdef DEARSQL_HAS_MONGODB
    case DatabaseType::MONGODB:
        return std::make_shared<MongoDBConnection>(info);
#endif
#ifdef DEARSQL_HAS_REDIS
    case DatabaseType::REDIS:
        return std::make_shared<RedisConnection>(info);
#endif
#ifdef DEARSQL_HAS_MSSQL
    case DatabaseType::MSSQL:
        return std::make_shared<MSSQLConnection>(info);
#endif
#ifdef DEARSQL_HAS_ORACLE
    case DatabaseType::ORACLE:
        return std::make_shared<OracleConnection>(info);
#endif
#ifdef DEARSQL_HAS_CASSANDRA
    case DatabaseType::CASSANDRA:
    case DatabaseType::SCYLLADB:
        return std::make_shared<CassandraConnection>(info);
#endif
#ifdef DEARSQL_HAS_DUCKDB
    case DatabaseType::DUCKDB:
        return std::make_shared<DuckDBConnection>(info);
#endif
    default:
        break;
    }
    return nullptr;
}

} // namespace dearsql
