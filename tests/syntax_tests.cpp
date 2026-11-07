#include <dearsql/syntax.hpp>
#include <gtest/gtest.h>

using namespace dearsql;

TEST(Syntax, PostgresAcceptsItsOwnDialect) {
    for (const char* sql : {
             "select id::text from users",
             "insert into t(a) values (1) on conflict (a) do nothing returning id",
             "select distinct on (a) a, b from t where data->>'name' = $1",
             "select count(*) filter (where a > 1), row_number() over (partition by a) from t",
             "update t set a = s.a from s where s.id = t.id",
             "create index concurrently idx on t (a); alter table t add column c int",
             "select * from t where active is true and id = any(array[1,2])",
             "with x as (select 1 as a) select a from x; select now() - interval '30 days';",
         }) {
        EXPECT_FALSE(checkSyntax(sql, DatabaseType::POSTGRESQL)) << sql;
    }
}

TEST(Syntax, PostgresPointsAtTheError) {
    const std::string sql = "select 1;\nselect * frm t";
    const auto err = checkSyntax(sql, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(err);
    EXPECT_NE(err->message.find("syntax error"), std::string::npos) << err->message;
    EXPECT_EQ(sql.substr(err->offset, 3), "frm"); // `*` takes no alias
    // characters, not bytes, come back from postgres: still lands on the token
    const std::string utf8 = "select 'héllo' frm t";
    const auto u = checkSyntax(utf8, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(u);
    EXPECT_EQ(utf8.substr(u->offset, 1), "t");
}

TEST(Syntax, SqliteIgnoresMissingTables) {
    for (const char* sql : {
             "select * from nowhere",
             "pragma table_info(t); select * from t where a glob 'x*'",
             "insert into t values (1) on conflict do nothing; select * from t limit 5, 10",
             "select 'a;b' from t; -- trailing",
         }) {
        EXPECT_FALSE(checkSyntax(sql, DatabaseType::SQLITE)) << sql;
    }
    const std::string sql = "select 1;\nselect * frm t";
    const auto err = checkSyntax(sql, DatabaseType::SQLITE);
    ASSERT_TRUE(err);
    EXPECT_EQ(sql.substr(err->offset, 3), "frm");
}

#ifdef DEARSQL_HAS_DUCKDB
TEST(Syntax, Duckdb) {
    EXPECT_FALSE(checkSyntax("select * from read_csv('a.csv') qualify row_number() over () = 1",
                             DatabaseType::DUCKDB));
    EXPECT_TRUE(checkSyntax("select * frm t", DatabaseType::DUCKDB));
}
#endif

TEST(Syntax, NoGuessingForOtherEngines) {
    EXPECT_FALSE(hasSyntaxCheck(DatabaseType::MYSQL));
    EXPECT_FALSE(checkSyntax("select * frm t", DatabaseType::MYSQL));
    EXPECT_FALSE(checkSyntax("select top 10 * from [t]", DatabaseType::MSSQL));
}

// SHOW SYNTAX output as cockroach 25 returns it: the caret points into the second statement
TEST(Syntax, CockroachShowSyntaxResult) {
    const std::string sql = "select 1;\nselect *\n  frm t";
    EXPECT_EQ(dearsql::cockroachSyntaxSql("select 'a'"), "SHOW SYNTAX 'select ''a'''");
    dearsql::StatementResult r;
    r.tableData = {{"error", "at or near \"frm\": syntax error"},
                   {"code", "42601"},
                   {"detail", "source SQL:\nselect *\n  frm t\n  ^"}};
    const auto error = dearsql::cockroachSyntaxError(sql, r);
    ASSERT_TRUE(error);
    EXPECT_EQ(error->message, "at or near \"frm\": syntax error");
    EXPECT_EQ(sql.substr(error->offset, 3), "frm");

    dearsql::StatementResult ok;
    ok.tableData = {{"sql", "SELECT 1"}, {"sql", "SELECT * FROM t"}};
    EXPECT_FALSE(dearsql::cockroachSyntaxError(sql, ok));
}

// 1064 messages as mysql 8 and tidb word them, read back to an offset in the statement
TEST(Syntax, MysqlParseErrorOffset) {
    const std::string stmt = "select *\nfrom t\nwhere x = = 1";
    const auto at = stmt.find("= 1");
    EXPECT_EQ(dearsql::mysqlParseErrorOffset(
                  stmt, "You have an error in your SQL syntax; check the manual that corresponds "
                        "to your MySQL server version for the right syntax to use near '= 1' at "
                        "line 3"),
              at);
    EXPECT_EQ(dearsql::mysqlParseErrorOffset(
                  stmt, "You have an error in your SQL syntax; check the manual that corresponds "
                        "to your TiDB version for the right syntax to use line 3 column 12 near "
                        "\"= 1\" "),
              at);
    EXPECT_EQ(dearsql::mysqlParseErrorOffset("select 1 from", "... to use near '' at line 1"), 13u);
    EXPECT_FALSE(dearsql::mysqlParseErrorOffset(stmt, "Table 't' doesn't exist"));
}
