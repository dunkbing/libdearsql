#include <dearsql/factory.hpp>
#include <dearsql/findings.hpp>
#include <dearsql/profile.hpp>
#include <dearsql/schema_diff.hpp>
#include <gtest/gtest.h>

using namespace dearsql;

namespace {
    Table users() {
        Table t{.name = "users", .schema = "public"};
        t.columns = {{.name = "id", .type = "integer", .isPrimaryKey = true, .isNotNull = true},
                     {.name = "email", .type = "text"},
                     {.name = "age", .type = "integer"}};
        t.indexes = {{.name = "users_email", .columns = {"email"}}};
        return t;
    }

    std::string all(const std::vector<SchemaChange>& changes) {
        std::string out;
        for (const auto& c : changes)
            out += c.message + "\n" + c.sql + "\n";
        return out;
    }
} // namespace

TEST(SchemaDiff, SameSchemaAndARenamedIndexAreNoChange) {
    auto other = users();
    other.indexes[0].name = "idx_users_email"; // same columns, other name
    EXPECT_TRUE(diffSchemas({users()}, {other}, DatabaseType::POSTGRESQL, "public").empty());
}

TEST(SchemaDiff, ColumnsIndexesAndTablesInRunOrder) {
    auto from = users();
    auto to = users();
    to.columns.erase(to.columns.begin() + 2);                      // drop age
    to.columns.push_back({.name = "name", .type = "varchar(80)"}); // add name
    to.columns[1].type = "varchar(255)";                           // email type
    to.indexes = {{.name = "users_name", .columns = {"name"}, .isUnique = true}};
    Table orders{.name = "orders", .schema = "public"};
    orders.columns = {{.name = "id", .type = "integer", .isPrimaryKey = true}};
    Table old{.name = "old_log", .schema = "public"};
    old.columns = {{.name = "id", .type = "integer"}};

    const auto changes = diffSchemas({from, old}, {to, orders}, DatabaseType::POSTGRESQL, "public");
    std::vector<SchemaChange::Kind> kinds;
    for (const auto& c : changes)
        kinds.push_back(c.kind);
    using K = SchemaChange::Kind;
    EXPECT_EQ(kinds, (std::vector<K>{K::CreateTable, K::AddColumn, K::AlterColumn, K::AddIndex,
                                     K::DropIndex, K::DropColumn, K::DropTable}))
        << all(changes);
    EXPECT_NE(changes[0].sql.find("CREATE TABLE \"public\".\"orders\""), std::string::npos);
    EXPECT_NE(changes[2].sql.find("TYPE varchar(255);"), std::string::npos) << changes[2].sql;
    EXPECT_NE(changes[3].sql.find("CREATE UNIQUE INDEX \"users_name\""), std::string::npos);
    EXPECT_EQ(changes[4].sql, "DROP INDEX \"public\".\"users_email\";");
    EXPECT_EQ(changes[6].sql, "DROP TABLE \"public\".\"old_log\";");
}

TEST(SchemaDiff, SqliteSaysWhatItCannotAlter) {
    auto from = users();
    from.schema.clear();
    auto to = from;
    to.columns[2].type = "real";
    to.foreignKeys = {
        {.name = "fk", .sourceColumn = "age", .targetTable = "ages", .targetColumn = "id"}};
    const auto changes = diffSchemas({from}, {to}, DatabaseType::SQLITE);
    ASSERT_EQ(changes.size(), 2u) << all(changes);
    EXPECT_TRUE(changes[0].sql.empty());
    EXPECT_NE(changes[0].message.find("rebuild the table"), std::string::npos);
    EXPECT_TRUE(changes[1].sql.empty());
    EXPECT_EQ(changes[1].kind, SchemaChange::Kind::AddForeignKey);
}

TEST(SchemaDiff, MySqlDropsNameTheTable) {
    EXPECT_EQ(dropIndexSql(DatabaseType::MYSQL, "i", "t"), "DROP INDEX i ON t;");
    EXPECT_EQ(dropIndexSql(DatabaseType::POSTGRESQL, "i", "t"), "DROP INDEX i;");
    EXPECT_TRUE(sameDialect(DatabaseType::POSTGRESQL, DatabaseType::COCKROACHDB));
    EXPECT_FALSE(sameDialect(DatabaseType::POSTGRESQL, DatabaseType::MYSQL));
    // index review uses the same form
    Table t{.name = "t"};
    t.columns = {{.name = "a", .type = "int"}};
    t.indexes = {{.name = "i1", .columns = {"a"}}, {.name = "i2", .columns = {"a"}}};
    const auto f = reviewIndexes({t}, DatabaseType::MYSQL);
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f[0].suggestion, "DROP INDEX i2 ON t;");
}

TEST(Profile, OneBoundedScanAndOnlySafeAggregates) {
    Table t{.name = "users"};
    t.columns = {{.name = "id", .type = "integer"},
                 {.name = "active", .type = "boolean"},
                 {.name = "doc", .type = "json"}};
    const auto sql = profileSql(DatabaseType::POSTGRESQL, t, 1000);
    ASSERT_TRUE(sql);
    EXPECT_EQ(*sql, "SELECT COUNT(*), COUNT(\"id\"), COUNT(DISTINCT \"id\"), MIN(\"id\"), "
                    "MAX(\"id\"), COUNT(\"active\"), COUNT(DISTINCT \"active\"), COUNT(\"doc\") "
                    "FROM (SELECT \"id\", \"active\", \"doc\" FROM \"users\" LIMIT 1000) s");
    StatementResult r;
    r.tableData = {{"1000", "1000", "990", "1", "1200", "998", "2", "10"}};
    const auto p = parseProfile(DatabaseType::POSTGRESQL, t, r, 1000);
    ASSERT_TRUE(p);
    EXPECT_TRUE(p->sampled);
    EXPECT_EQ(p->columns[0].max, "1200");
    EXPECT_EQ(p->columns[1].distinct, 2);
    EXPECT_FALSE(p->columns[1].hasMinMax);
    EXPECT_EQ(p->columns[2].distinct, -1);
    EXPECT_EQ(p->columns[2].nonNull, 10);

    EXPECT_FALSE(profileSql(DatabaseType::MONGODB, t));
    EXPECT_FALSE(topValuesSql(DatabaseType::POSTGRESQL, t, t.columns[2]));
    EXPECT_EQ(*topValuesSql(DatabaseType::MSSQL, t, t.columns[1], 3, 10),
              "SELECT TOP 3 [active], COUNT(*) AS n FROM (SELECT TOP 10 [active] FROM [users]) s "
              "GROUP BY [active] ORDER BY COUNT(*) DESC");
}

TEST(Profile, RowsPerColumnRunOnSqlite) {
    ConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.path = ":memory:";
    auto conn = makeConnection(info);
    ASSERT_TRUE(conn->open().first);
    auto db = conn->database();
    ASSERT_TRUE(db->execute("CREATE TABLE t (id INTEGER, tag TEXT, doc JSON);"
                            "INSERT INTO t VALUES (1, 'a', NULL), (2, 'a', NULL), (3, NULL, '{}');",
                            0)
                    .success());
    Table t{.name = "t"};
    t.columns = {{.name = "id", .type = "INTEGER"},
                 {.name = "tag", .type = "TEXT"},
                 {.name = "doc", .type = "JSON"}};
    const auto r = db->execute(*profileRowsSql(DatabaseType::SQLITE, t), 0);
    ASSERT_TRUE(r.success()) << r.errorMessage();
    const auto& rows = r.statements.back().tableData;
    ASSERT_EQ(rows.size(), 3u);
    // column, type, nulls, distinct, min, max, rows
    EXPECT_EQ(rows[0], (std::vector<std::string>{"id", "INTEGER", "0", "3", "1", "3", "3"}));
    EXPECT_EQ(rows[1][2], "1");
    EXPECT_EQ(rows[1][3], "1");
    EXPECT_TRUE(isNullSentinel(rows[2][3])); // json: no distinct
}
