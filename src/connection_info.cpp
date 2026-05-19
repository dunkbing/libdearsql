#include "dearsql/connection_info.hpp"

namespace dearsql {

std::string sslModeToString(SslMode mode) {
    switch (mode) {
    case SslMode::Disable:
        return "disable";
    case SslMode::Allow:
        return "allow";
    case SslMode::Prefer:
        return "prefer";
    case SslMode::Require:
        return "require";
    case SslMode::VerifyCA:
        return "verify-ca";
    case SslMode::VerifyFull:
        return "verify-full";
    case SslMode::VerifyIdentity:
        return "verify-identity";
    }
    return "prefer";
}

SslMode stringToSslMode(const std::string& str) {
    if (str == "disable")
        return SslMode::Disable;
    if (str == "allow")
        return SslMode::Allow;
    if (str == "prefer")
        return SslMode::Prefer;
    if (str == "require")
        return SslMode::Require;
    if (str == "verify-ca")
        return SslMode::VerifyCA;
    if (str == "verify-full")
        return SslMode::VerifyFull;
    if (str == "verify-identity")
        return SslMode::VerifyIdentity;
    return SslMode::Prefer;
}

std::string databaseTypeToString(DatabaseType type) {
    switch (type) {
    case DatabaseType::SQLITE:
        return "sqlite";
    case DatabaseType::POSTGRESQL:
        return "postgresql";
    case DatabaseType::MYSQL:
        return "mysql";
    case DatabaseType::MARIADB:
        return "mariadb";
    case DatabaseType::REDIS:
        return "redis";
    case DatabaseType::MONGODB:
        return "mongodb";
    case DatabaseType::MSSQL:
        return "mssql";
    case DatabaseType::ORACLE:
        return "oracle";
    case DatabaseType::REDSHIFT:
        return "redshift";
    case DatabaseType::CASSANDRA:
        return "cassandra";
    }
    return "unknown";
}

DatabaseType stringToDatabaseType(const std::string& s) {
    if (s == "sqlite")
        return DatabaseType::SQLITE;
    if (s == "postgresql" || s == "postgres")
        return DatabaseType::POSTGRESQL;
    if (s == "mysql")
        return DatabaseType::MYSQL;
    if (s == "mariadb")
        return DatabaseType::MARIADB;
    if (s == "redis")
        return DatabaseType::REDIS;
    if (s == "mongodb" || s == "mongo")
        return DatabaseType::MONGODB;
    if (s == "mssql")
        return DatabaseType::MSSQL;
    if (s == "oracle")
        return DatabaseType::ORACLE;
    if (s == "redshift")
        return DatabaseType::REDSHIFT;
    if (s == "cassandra")
        return DatabaseType::CASSANDRA;
    return DatabaseType::SQLITE;
}

std::string ConnectionInfo::buildConnectionString(const std::string& dbName) const {
    switch (type) {
    case DatabaseType::SQLITE:
        return path;

    case DatabaseType::REDSHIFT:
    case DatabaseType::POSTGRESQL: {
        std::string connStr = "host=" + host + " port=" + std::to_string(port);
        connStr += " connect_timeout=10";

        if (!dbName.empty()) {
            connStr += " dbname=" + dbName;
        } else if (!database.empty()) {
            connStr += " dbname=" + database;
        } else {
            connStr +=
                std::string(" dbname=") + (type == DatabaseType::REDSHIFT ? "dev" : "postgres");
        }

        if (!username.empty())
            connStr += " user=" + username;
        if (!password.empty())
            connStr += " password=" + password;

        connStr += " sslmode=" + sslModeToString(sslmode);
        if ((sslmode == SslMode::VerifyCA || sslmode == SslMode::VerifyFull) &&
            !sslCACertPath.empty()) {
            connStr += " sslrootcert=" + sslCACertPath;
        }
        return connStr;
    }

    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB: {
        const std::string targetDb = !dbName.empty() ? dbName : database;
        std::string connStr =
            "host=" + host + " port=" + std::to_string(port) + " dbname=" + targetDb;
        if (!username.empty())
            connStr += " user=" + username;
        if (!password.empty())
            connStr += " password=" + password;
        return connStr;
    }

    case DatabaseType::REDIS:
        return "redis://" + host + ":" + std::to_string(port);

    case DatabaseType::MONGODB: {
        std::string connStr = "mongodb://";
        if (!username.empty()) {
            connStr += username;
            if (!password.empty())
                connStr += ":" + password;
            connStr += "@";
        }
        connStr += host + ":" + std::to_string(port);
        if (!dbName.empty())
            connStr += "/" + dbName;
        else if (!database.empty())
            connStr += "/" + database;

        if (sslmode == SslMode::Require || sslmode == SslMode::VerifyCA ||
            sslmode == SslMode::VerifyFull) {
            connStr += (connStr.find('?') != std::string::npos) ? "&" : "?";
            connStr += "tls=true";
            if (!sslCACertPath.empty())
                connStr += "&tlsCAFile=" + sslCACertPath;
            else if (sslmode == SslMode::Require)
                connStr += "&tlsAllowInvalidCertificates=true";
        }
        return connStr;
    }

    case DatabaseType::MSSQL:
        return host + ":" + std::to_string(port);

    case DatabaseType::ORACLE:
        return "//" + host + ":" + std::to_string(port) + "/" + database;

    case DatabaseType::CASSANDRA:
        return host + ":" + std::to_string(port);
    }
    return "";
}

} // namespace dearsql
