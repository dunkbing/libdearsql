#pragma once

#include <string>

namespace dearsql {

enum class DatabaseType {
    SQLITE,
    POSTGRESQL,
    MYSQL,
    MARIADB,
    REDIS,
    MONGODB,
    MSSQL,
    ORACLE,
    REDSHIFT,
    CASSANDRA,
    DUCKDB
};

enum class SslMode { Disable, Allow, Prefer, Require, VerifyCA, VerifyFull, VerifyIdentity };

std::string sslModeToString(SslMode mode);
SslMode stringToSslMode(const std::string& str);
// .csv files open through DuckDB as an in-memory table
bool isCsvPath(const std::string& path);
std::string databaseTypeToString(DatabaseType type);
DatabaseType stringToDatabaseType(const std::string& typeStr);

// connection info shared by all backends; SSH-free (the app handles tunneling
// and rewrites host/port to the local forwarded endpoint before opening).
struct ConnectionInfo {
    DatabaseType type = DatabaseType::SQLITE;
    std::string name;           // user-friendly connection name
    std::string path;           // SQLite file path
    std::string host;
    int port = 5432;
    std::string database;       // default database/keyspace/service
    std::string username;
    std::string password;
    bool showAllDatabases = false;
    SslMode sslmode = SslMode::Prefer;
    std::string sslCACertPath;  // CA cert / Oracle wallet path
    // file backends open read-only; servers rely on the host refusing writes
    bool readOnly = false;

    [[nodiscard]] std::string buildConnectionString(const std::string& dbName = "") const;
};

} // namespace dearsql
