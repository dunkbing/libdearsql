#include "dearsql/connection_info.hpp"

#include <cctype>

namespace dearsql {
namespace {

std::string escapeLibpqValue(const std::string& value) {
    std::string escaped = "'";
    for (char ch : value) {
        if (ch == '\'' || ch == '\\')
            escaped += '\\';
        escaped += ch;
    }
    escaped += "'";
    return escaped;
}

void appendLibpqKeyword(std::string& connStr, const std::string& key, const std::string& value) {
    connStr += " " + key + "=" + escapeLibpqValue(value);
}

std::string uriEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out += static_cast<char>(ch);
        } else {
            out += '%';
            out += hex[ch >> 4];
            out += hex[ch & 0x0F];
        }
    }
    return out;
}

} // namespace

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
        std::string connStr = "host=" + escapeLibpqValue(host) + " port=" + std::to_string(port);
        connStr += " connect_timeout=10";

        if (!dbName.empty()) {
            appendLibpqKeyword(connStr, "dbname", dbName);
        } else if (!database.empty()) {
            appendLibpqKeyword(connStr, "dbname", database);
        } else {
            appendLibpqKeyword(connStr, "dbname",
                               type == DatabaseType::REDSHIFT ? "dev" : "postgres");
        }

        if (!username.empty())
            appendLibpqKeyword(connStr, "user", username);
        if (!password.empty())
            appendLibpqKeyword(connStr, "password", password);

        connStr += " sslmode=" + sslModeToString(sslmode);
        if ((sslmode == SslMode::VerifyCA || sslmode == SslMode::VerifyFull) &&
            !sslCACertPath.empty()) {
            appendLibpqKeyword(connStr, "sslrootcert", sslCACertPath);
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
            connStr += uriEncode(username);
            if (!password.empty())
                connStr += ":" + uriEncode(password);
            connStr += "@";
        }
        connStr += host + ":" + std::to_string(port);
        if (!dbName.empty())
            connStr += "/" + uriEncode(dbName);
        else if (!database.empty())
            connStr += "/" + uriEncode(database);

        if (sslmode == SslMode::Require || sslmode == SslMode::VerifyCA ||
            sslmode == SslMode::VerifyFull) {
            connStr += (connStr.find('?') != std::string::npos) ? "&" : "?";
            connStr += "tls=true";
            if (!sslCACertPath.empty())
                connStr += "&tlsCAFile=" + uriEncode(sslCACertPath);
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
