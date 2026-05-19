#include "dearsql/factory.hpp"

#include "dearsql/backends/cassandra_connection.hpp"
#include "dearsql/backends/mongodb_connection.hpp"
#include "dearsql/backends/mssql_connection.hpp"
#include "dearsql/backends/mysql_connection.hpp"
#include "dearsql/backends/oracle_connection.hpp"
#include "dearsql/backends/postgres_connection.hpp"
#include "dearsql/backends/redis_connection.hpp"
#include "dearsql/backends/sqlite_connection.hpp"

namespace dearsql {

ConnectionPtr makeConnection(const ConnectionInfo& info) {
    switch (info.type) {
    case DatabaseType::SQLITE:
        return std::make_shared<SQLiteConnection>(info);
    case DatabaseType::POSTGRESQL:
    case DatabaseType::REDSHIFT:
        return std::make_shared<PostgresConnection>(info);
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        return std::make_shared<MySQLConnection>(info);
    case DatabaseType::MONGODB:
        return std::make_shared<MongoDBConnection>(info);
    case DatabaseType::REDIS:
        return std::make_shared<RedisConnection>(info);
    case DatabaseType::MSSQL:
        return std::make_shared<MSSQLConnection>(info);
    case DatabaseType::ORACLE:
        return std::make_shared<OracleConnection>(info);
    case DatabaseType::CASSANDRA:
        return std::make_shared<CassandraConnection>(info);
    }
    return nullptr;
}

} // namespace dearsql
