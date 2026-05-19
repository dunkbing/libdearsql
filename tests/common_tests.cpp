#include "dearsql/dearsql.hpp"
#include <gtest/gtest.h>

using namespace dearsql;

TEST(ConnectionInfo, BuildPostgresConnectionString) {
    ConnectionInfo info;
    info.type = DatabaseType::POSTGRESQL;
    info.host = "localhost";
    info.port = 5432;
    info.database = "app";
    info.username = "u";
    info.password = "p";
    info.sslmode = SslMode::Disable;

    auto s = info.buildConnectionString();
    EXPECT_NE(s.find("host=localhost"), std::string::npos);
    EXPECT_NE(s.find("port=5432"), std::string::npos);
    EXPECT_NE(s.find("dbname=app"), std::string::npos);
    EXPECT_NE(s.find("user=u"), std::string::npos);
    EXPECT_NE(s.find("password=p"), std::string::npos);
    EXPECT_NE(s.find("sslmode=disable"), std::string::npos);
}

TEST(ConnectionInfo, BuildMongoConnectionString) {
    ConnectionInfo info;
    info.type = DatabaseType::MONGODB;
    info.host = "h";
    info.port = 27017;
    info.username = "u";
    info.password = "p";
    info.database = "d";

    auto s = info.buildConnectionString();
    EXPECT_EQ(s, "mongodb://u:p@h:27017/d");
}

TEST(ConnectionInfo, BuildSqliteConnectionString) {
    ConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.path = "/tmp/foo.db";
    EXPECT_EQ(info.buildConnectionString(), "/tmp/foo.db");
}

TEST(EnumConversions, DatabaseTypeRoundTrip) {
    for (auto t : {DatabaseType::SQLITE, DatabaseType::POSTGRESQL, DatabaseType::MYSQL,
                   DatabaseType::MARIADB, DatabaseType::REDIS, DatabaseType::MONGODB,
                   DatabaseType::MSSQL, DatabaseType::ORACLE, DatabaseType::REDSHIFT,
                   DatabaseType::CASSANDRA}) {
        EXPECT_EQ(stringToDatabaseType(databaseTypeToString(t)), t);
    }
}

TEST(EnumConversions, SslModeRoundTrip) {
    for (auto m : {SslMode::Disable, SslMode::Allow, SslMode::Prefer, SslMode::Require,
                   SslMode::VerifyCA, SslMode::VerifyFull, SslMode::VerifyIdentity}) {
        EXPECT_EQ(stringToSslMode(sslModeToString(m)), m);
    }
}

TEST(ForeignKeyLookup, BuildAndPopulate) {
    Table parent;
    parent.name = "users";
    Column id;
    id.name = "id";
    id.isPrimaryKey = true;
    parent.columns.push_back(id);

    Table child;
    child.name = "orders";
    Column userId;
    userId.name = "user_id";
    child.columns.push_back(userId);

    ForeignKey fk;
    fk.name = "fk_orders_user";
    fk.sourceColumn = "user_id";
    fk.targetTable = "users";
    fk.targetColumn = "id";
    child.foreignKeys.push_back(fk);

    buildForeignKeyLookup(child);
    ASSERT_TRUE(child.foreignKeysByColumn.count("user_id"));
    EXPECT_EQ(child.foreignKeysByColumn.at("user_id").targetTable, "users");

    std::vector<Table> tables{parent, child};
    populateIncomingForeignKeys(tables);
    EXPECT_EQ(tables[0].incomingForeignKeys.size(), 1u);
    EXPECT_EQ(tables[0].incomingForeignKeys.front().sourceColumn, "user_id");
}

TEST(ByteFormatting, HumanReadable) {
    EXPECT_EQ(formatByteSize(0), "0 B");
    EXPECT_EQ(formatByteSize(512), "512 B");
    EXPECT_EQ(formatByteSize(1024), "1.0 KB");
    EXPECT_EQ(formatByteSize(2 * 1024 * 1024), "2.0 MB");
    EXPECT_TRUE(formatByteSize(-1).empty());
}
