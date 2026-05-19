#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_ORACLE_HOST", "DEARSQL_TEST_ORACLE_PORT",
                        "DEARSQL_TEST_ORACLE_DB", "DEARSQL_TEST_ORACLE_USER",
                        "DEARSQL_TEST_ORACLE_PASSWORD", "DEARSQL_TEST_ORACLE_NAME", 1521);
    if (c && c->name.empty())
        c->name = "OracleDocker";
    if (c && c->database.empty())
        c->database = "FREEPDB1";
    return c;
}

#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_ORACLE_HOST");                                      \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::ORACLE);                               \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second
} // namespace

TEST(Oracle, OpenAndListDatabases) {
    OPEN_OR_SKIP(conn, st);
    ASSERT_TRUE(conn->isOpen());
    EXPECT_FALSE(conn->databases().empty());
}

TEST(Oracle, OpenAutoDetectsServiceWhenBlank) {
    auto c = cfg();
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_ORACLE_HOST");
    c->database.clear();
    auto [conn, st] = tryOpen(*c, DatabaseType::ORACLE);
    DEARSQL_SKIP_IF_UNIMPLEMENTED(st);
    ASSERT_TRUE(st.first) << st.second;
    ASSERT_TRUE(conn->isOpen());
    auto r = conn->database()->execute("SELECT 1 AS one FROM dual");
    ASSERT_TRUE(r.success()) << r.errorMessage();
}

TEST(Oracle, SelectFromDual) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT 1 AS one FROM dual");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    EXPECT_EQ(r[0].tableData[0][0], "1");
}

TEST(Oracle, ExecuteErrorReturnsFailure) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT * FROM dearsql_does_not_exist_xyz");
    EXPECT_FALSE(r.success());
}

TEST(Oracle, NullSentinelInResult) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT NULL AS v FROM dual");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    EXPECT_TRUE(isNullSentinel(r[0].tableData[0][0]));
}

TEST(Oracle, RoundTripTable) {
    OPEN_OR_SKIP(conn, st);
    auto db = conn->database();
    ASSERT_TRUE(db);
    const std::string tbl = "DEARSQL_LIB_ORA_ROUND";
    db->execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + tbl +
                "'; EXCEPTION WHEN OTHERS THEN NULL; END;");
    {
        auto r = db->execute("CREATE TABLE " + tbl + " (id NUMBER PRIMARY KEY, name VARCHAR2(64))");
        ASSERT_TRUE(r.success()) << r.errorMessage();
    }
    db->execute("INSERT INTO " + tbl + " VALUES (1,'a')");
    db->execute("INSERT INTO " + tbl + " VALUES (2,'b')");

    Table t;
    t.name = tbl;
    EXPECT_EQ(db->getRowCount(t), 2);

    auto rows = db->getTableData(t, 10, 0, "", "id ASC");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0], "1");

    db->execute("DROP TABLE " + tbl);
}

TEST(Oracle, DescribeTable) {
    OPEN_OR_SKIP(conn, st);
    auto db = conn->database();
    const std::string tbl = "DEARSQL_LIB_ORA_DESC";
    db->execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + tbl +
                "'; EXCEPTION WHEN OTHERS THEN NULL; END;");
    db->execute("CREATE TABLE " + tbl +
                " (id NUMBER PRIMARY KEY, name VARCHAR2(64) NOT NULL, qty NUMBER)");
    auto t = db->describeTable(tbl);
    EXPECT_EQ(t.columns.size(), 3u);
    EXPECT_TRUE(t.columns[0].isPrimaryKey);
    EXPECT_TRUE(t.columns[1].isNotNull);
    db->execute("DROP TABLE " + tbl);
}

TEST(Oracle, CreateAndDropTableViaApi) {
    OPEN_OR_SKIP(conn, st);
    auto db = conn->database();
    const std::string tbl = "DEARSQL_LIB_ORA_CREATE";
    db->execute("BEGIN EXECUTE IMMEDIATE 'DROP TABLE " + tbl +
                "'; EXCEPTION WHEN OTHERS THEN NULL; END;");

    Table t;
    t.name = tbl;
    Column id;
    id.name = "ID";
    id.type = "NUMBER";
    id.isPrimaryKey = true;
    Column name;
    name.name = "NAME";
    name.type = "VARCHAR2(64)";
    name.isNotNull = true;
    t.columns = {id, name};
    auto [ok, err] = db->createTable(t);
    ASSERT_TRUE(ok) << err;

    auto [okd, errd] = db->dropTable(tbl);
    ASSERT_TRUE(okd) << errd;
}

TEST(Oracle, Sequences) {
    OPEN_OR_SKIP(conn, st);
    auto db = conn->database();
    db->execute("BEGIN EXECUTE IMMEDIATE 'DROP SEQUENCE DEARSQL_LIB_ORA_SEQ'; "
                "EXCEPTION WHEN OTHERS THEN NULL; END;");
    auto r = db->execute("CREATE SEQUENCE DEARSQL_LIB_ORA_SEQ START WITH 100");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    auto seqs = db->sequences();
    bool found = false;
    for (const auto& s : seqs)
        if (s == "DEARSQL_LIB_ORA_SEQ")
            found = true;
    EXPECT_TRUE(found);
    db->execute("DROP SEQUENCE DEARSQL_LIB_ORA_SEQ");
}
