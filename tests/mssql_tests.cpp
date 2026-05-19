#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_MSSQL_HOST", "DEARSQL_TEST_MSSQL_PORT",
                        "DEARSQL_TEST_MSSQL_DB", "DEARSQL_TEST_MSSQL_USER",
                        "DEARSQL_TEST_MSSQL_PASSWORD", "DEARSQL_TEST_MSSQL_NAME", 1433);
    if (c && c->name.empty())
        c->name = "MSSQLDocker";
    if (c && c->database.empty())
        c->database = "master";
    return c;
}

DatabasePtr dboSchema(ConnectionPtr conn, const std::string& dbName) {
    auto db = conn->database(dbName);
    if (!db)
        return nullptr;
    for (auto& s : db->schemas())
        if (s && s->name() == "dbo")
            return s;
    return nullptr;
}

#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_MSSQL_HOST");                                       \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::MSSQL);                                \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second
} // namespace

TEST(MSSQL, OpenAndListDatabases) {
    OPEN_OR_SKIP(conn, st);
    ASSERT_TRUE(conn->isOpen());
    EXPECT_FALSE(conn->databases().empty());
}

TEST(MSSQL, ExecuteSelect) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT 1 AS one");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    EXPECT_EQ(r[0].tableData[0][0], "1");
}

TEST(MSSQL, ExecuteErrorReturnsFailure) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT * FROM dearsql_does_not_exist_xyz");
    EXPECT_FALSE(r.success());
}

TEST(MSSQL, NullSentinelInResult) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT NULL AS v");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    EXPECT_TRUE(isNullSentinel(r[0].tableData[0][0]));
}

TEST(MSSQL, ListSchemas) {
    OPEN_OR_SKIP(conn, st);
    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    auto schemas = db->schemas();
    EXPECT_FALSE(schemas.empty()); // at least dbo, sys
    bool foundDbo = false;
    for (auto& s : schemas)
        if (s && s->name() == "dbo")
            foundDbo = true;
    EXPECT_TRUE(foundDbo);
}

TEST(MSSQL, RoundTripTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mssql_round";
    conn->database()->execute("IF OBJECT_ID('dbo." + tbl + "','U') IS NOT NULL DROP TABLE dbo." + tbl);
    {
        auto r = conn->database()->execute(
            "CREATE TABLE dbo." + tbl + " (id INT PRIMARY KEY, name NVARCHAR(64) NOT NULL)");
        ASSERT_TRUE(r.success()) << r.errorMessage();
    }
    conn->database()->execute("INSERT INTO dbo." + tbl + " VALUES (1,'a'),(2,'b')");

    auto schema = dboSchema(conn, c->database);
    ASSERT_TRUE(schema);
    Table t;
    t.name = tbl;
    t.schema = "dbo";
    EXPECT_EQ(schema->getRowCount(t), 2);

    auto rows = schema->getTableData(t, 10, 0, "", "id ASC");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0], "1");

    conn->database()->execute("DROP TABLE dbo." + tbl);
}

TEST(MSSQL, DescribeTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mssql_desc";
    conn->database()->execute("IF OBJECT_ID('dbo." + tbl + "','U') IS NOT NULL DROP TABLE dbo." + tbl);
    conn->database()->execute(
        "CREATE TABLE dbo." + tbl +
        " (id INT IDENTITY PRIMARY KEY, name NVARCHAR(64) NOT NULL, qty INT NULL)");

    auto schema = dboSchema(conn, c->database);
    ASSERT_TRUE(schema);
    auto t = schema->describeTable(tbl);
    EXPECT_EQ(t.columns.size(), 3u);
    EXPECT_TRUE(t.columns[0].isPrimaryKey);

    conn->database()->execute("DROP TABLE dbo." + tbl);
}

TEST(MSSQL, CreateAndDropTableViaApi) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_mssql_create";
    auto schema = dboSchema(conn, c->database);
    ASSERT_TRUE(schema);
    schema->dropTable(tbl);

    Table t;
    t.name = tbl;
    t.schema = "dbo";
    Column id;
    id.name = "id";
    id.type = "INT";
    id.isPrimaryKey = true;
    Column name;
    name.name = "name";
    name.type = "NVARCHAR(64)";
    name.isNotNull = true;
    t.columns = {id, name};
    auto [ok, err] = schema->createTable(t);
    ASSERT_TRUE(ok) << err;

    schema->dropTable(tbl);
}

TEST(MSSQL, RenameTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string oldT = "dearsql_lib_mssql_old";
    const std::string newT = "dearsql_lib_mssql_new";
    conn->database()->execute("IF OBJECT_ID('dbo." + oldT + "','U') IS NOT NULL DROP TABLE dbo." + oldT);
    conn->database()->execute("IF OBJECT_ID('dbo." + newT + "','U') IS NOT NULL DROP TABLE dbo." + newT);
    conn->database()->execute("CREATE TABLE dbo." + oldT + " (id INT PRIMARY KEY)");
    auto schema = dboSchema(conn, c->database);
    auto [ok, err] = schema->renameTable(oldT, newT);
    ASSERT_TRUE(ok) << err;
    conn->database()->execute("DROP TABLE dbo." + newT);
}

TEST(MSSQL, CreateAndDropDatabase) {
    OPEN_OR_SKIP(conn, st);
    const std::string tmp = "dearsql_lib_mssql_tmpdb";
    conn->dropDatabase(tmp);
    CreateDatabaseOptions opts;
    opts.name = tmp;
    auto [ok, err] = conn->createDatabase(opts);
    ASSERT_TRUE(ok) << err;
    auto [okd, errd] = conn->dropDatabase(tmp);
    ASSERT_TRUE(okd) << errd;
}
