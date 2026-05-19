#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_MYSQL_HOST", "DEARSQL_TEST_MYSQL_PORT",
                        "DEARSQL_TEST_MYSQL_DB", "DEARSQL_TEST_MYSQL_USER",
                        "DEARSQL_TEST_MYSQL_PASSWORD", "DEARSQL_TEST_MYSQL_NAME", 3306);
    if (c && c->name.empty())
        c->name = "MySQLDocker";
    return c;
}

#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_MYSQL_HOST");                                       \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::MYSQL);                                \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second
} // namespace

TEST(MySQL, OpenAndListDatabases) {
    OPEN_OR_SKIP(conn, st);
    ASSERT_TRUE(conn->isOpen());
    auto dbs = conn->databases();
    EXPECT_FALSE(dbs.empty());
    bool foundTestDb = false;
    for (auto& d : dbs)
        if (d && d->name() == c->database)
            foundTestDb = true;
    EXPECT_TRUE(foundTestDb);
}

TEST(MySQL, ExecuteSelect) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT 1 AS one, 'hi' AS greeting");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].tableData[0][0], "1");
    EXPECT_EQ(r[0].tableData[0][1], "hi");
}

TEST(MySQL, ExecuteErrorReturnsFailure) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT * FROM dearsql_does_not_exist_xyz");
    EXPECT_FALSE(r.success());
    EXPECT_FALSE(r.errorMessage().empty());
}

TEST(MySQL, NullSentinelInResult) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT NULL AS v");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    EXPECT_TRUE(isNullSentinel(r[0].tableData[0][0]));
}

TEST(MySQL, NoSchemaLayer) {
    OPEN_OR_SKIP(conn, st);
    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    EXPECT_TRUE(db->schemas().empty());
}

TEST(MySQL, ListTablesAndViewsAfterCreate) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mysql_lt";
    conn->database()->execute("DROP VIEW IF EXISTS " + tbl + "_view");
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY, name VARCHAR(64))");
    conn->database()->execute("CREATE VIEW " + tbl + "_view AS SELECT id FROM " + tbl);

    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    bool foundTable = false, foundView = false;
    for (const auto& t : db->tables())
        if (t.name == tbl)
            foundTable = true;
    for (const auto& v : db->views())
        if (v.name == tbl + "_view")
            foundView = true;
    EXPECT_TRUE(foundTable);
    EXPECT_TRUE(foundView);

    conn->database()->execute("DROP VIEW " + tbl + "_view");
    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(MySQL, DescribeTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mysql_desc";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY AUTO_INCREMENT, "
                                                      "name VARCHAR(64) NOT NULL UNIQUE)");

    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    auto t = db->describeTable(tbl);
    EXPECT_EQ(t.columns.size(), 2u);
    EXPECT_EQ(t.columns[0].name, "id");
    EXPECT_TRUE(t.columns[0].isPrimaryKey);
    EXPECT_TRUE(t.columns[0].isAutoIncrement);
    EXPECT_TRUE(t.columns[1].isNotNull);

    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(MySQL, GetTableDataAndRowCount) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mysql_data";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY, name VARCHAR(64))");
    conn->database()->execute("INSERT INTO " + tbl + " VALUES (1,'a'),(2,'b'),(3,'c')");

    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    Table t;
    t.name = tbl;
    EXPECT_EQ(db->getRowCount(t), 3);
    EXPECT_EQ(db->getRowCount(t, "id > 1"), 2);

    auto names = db->getColumnNames(t);
    ASSERT_EQ(names.size(), 2u);

    auto rows = db->getTableData(t, 2, 0, "", "id ASC");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0], "1");

    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(MySQL, CreateTableViaApiAndDrop) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mysql_create";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);

    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    Table t;
    t.name = tbl;
    Column id;
    id.name = "id";
    id.type = "INT";
    id.isPrimaryKey = true;
    Column name;
    name.name = "name";
    name.type = "VARCHAR(64)";
    name.isNotNull = true;
    t.columns = {id, name};
    auto [ok, err] = db->createTable(t);
    ASSERT_TRUE(ok) << err;

    auto desc = db->describeTable(tbl);
    EXPECT_EQ(desc.columns.size(), 2u);

    auto [okd, errd] = db->dropTable(tbl);
    ASSERT_TRUE(okd) << errd;
}

TEST(MySQL, RenameTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string oldT = "dearsql_lib_mysql_old";
    const std::string newT = "dearsql_lib_mysql_new";
    conn->database()->execute("DROP TABLE IF EXISTS " + oldT);
    conn->database()->execute("DROP TABLE IF EXISTS " + newT);
    conn->database()->execute("CREATE TABLE " + oldT + " (id INT PRIMARY KEY)");

    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    auto [ok, err] = db->renameTable(oldT, newT);
    ASSERT_TRUE(ok) << err;
    conn->database()->execute("DROP TABLE " + newT);
}

TEST(MySQL, TruncateTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mysql_trunc";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY)");
    conn->database()->execute("INSERT INTO " + tbl + " VALUES (1),(2)");
    auto db = conn->database(c->database);
    Table t;
    t.name = tbl;
    EXPECT_EQ(db->getRowCount(t), 2);
    auto [ok, err] = db->truncateTable(tbl);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(db->getRowCount(t), 0);
    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(MySQL, Routines) {
    OPEN_OR_SKIP(conn, st);
    conn->database()->execute("DROP FUNCTION IF EXISTS dearsql_lib_mysql_fn");
    conn->database()->execute(
        "CREATE FUNCTION dearsql_lib_mysql_fn(x INT) RETURNS INT DETERMINISTIC RETURN x+1");
    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    bool found = false;
    for (const auto& fn : db->routines())
        if (fn.name == "dearsql_lib_mysql_fn")
            found = true;
    EXPECT_TRUE(found);
    conn->database()->execute("DROP FUNCTION dearsql_lib_mysql_fn");
}

TEST(MySQL, CreateAndDropDatabase) {
    OPEN_OR_SKIP(conn, st);
    const std::string tmp = "dearsql_lib_mysql_tmpdb";
    conn->dropDatabase(tmp);
    CreateDatabaseOptions opts;
    opts.name = tmp;
    auto [ok, err] = conn->createDatabase(opts);
    ASSERT_TRUE(ok) << err;
    auto [okd, errd] = conn->dropDatabase(tmp);
    ASSERT_TRUE(okd) << errd;
}
