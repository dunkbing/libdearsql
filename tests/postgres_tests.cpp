#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_PG_HOST", "DEARSQL_TEST_PG_PORT",
                        "DEARSQL_TEST_PG_DB", "DEARSQL_TEST_PG_USER",
                        "DEARSQL_TEST_PG_PASSWORD", "DEARSQL_TEST_PG_NAME", 5432);
    if (c && c->name.empty())
        c->name = "PostgresDocker";
    return c;
}

// Returns the IDatabase representing the `public` schema, asserting it exists.
DatabasePtr publicSchema(ConnectionPtr conn) {
    for (auto& s : conn->database()->schemas()) {
        if (s && s->name() == "public")
            return s;
    }
    return nullptr;
}

#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_PG_HOST");                                          \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::POSTGRESQL);                           \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second
} // namespace

TEST(Postgres, OpenAndListDatabases) {
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

TEST(Postgres, ExecuteSelect) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT 1 AS one, 'hi' AS greeting");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].columnNames.size(), 2u);
    ASSERT_FALSE(r[0].tableData.empty());
    EXPECT_EQ(r[0].tableData[0][0], "1");
    EXPECT_EQ(r[0].tableData[0][1], "hi");
}

TEST(Postgres, ExecuteErrorReturnsFailure) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT * FROM dearsql_does_not_exist_xyz");
    EXPECT_FALSE(r.success());
    EXPECT_FALSE(r.errorMessage().empty());
}

TEST(Postgres, NullSentinelInResult) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT NULL AS v");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    ASSERT_FALSE(r[0].tableData.empty());
    EXPECT_TRUE(isNullSentinel(r[0].tableData[0][0]));
}

TEST(Postgres, ListSchemas) {
    OPEN_OR_SKIP(conn, st);
    auto schemas = conn->database()->schemas();
    EXPECT_FALSE(schemas.empty());
    EXPECT_TRUE(publicSchema(conn) != nullptr);
}

TEST(Postgres, ListTablesAndViewsAfterCreate) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_pg_lt";
    conn->database()->execute("DROP VIEW IF EXISTS " + tbl + "_view");
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY, name TEXT NOT NULL)");
    conn->database()->execute("CREATE VIEW " + tbl + "_view AS SELECT id FROM " + tbl);

    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    bool foundTable = false, foundView = false;
    for (const auto& t : schema->tables())
        if (t.name == tbl)
            foundTable = true;
    for (const auto& v : schema->views())
        if (v.name == tbl + "_view")
            foundView = true;
    EXPECT_TRUE(foundTable);
    EXPECT_TRUE(foundView);

    conn->database()->execute("DROP VIEW " + tbl + "_view");
    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(Postgres, DescribeTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_pg_desc";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute(
        "CREATE TABLE " + tbl +
        " (id INT PRIMARY KEY, name TEXT NOT NULL, parent_id INT REFERENCES " + tbl + "(id))");

    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    auto t = schema->describeTable(tbl);
    EXPECT_EQ(t.columns.size(), 3u);
    EXPECT_EQ(t.columns[0].name, "id");
    EXPECT_TRUE(t.columns[0].isPrimaryKey);
    EXPECT_TRUE(t.columns[1].isNotNull);
    EXPECT_FALSE(t.foreignKeys.empty());

    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(Postgres, GetTableDataAndRowCount) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_pg_data";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY, name TEXT)");
    conn->database()->execute("INSERT INTO " + tbl + " VALUES (1,'a'),(2,'b'),(3,'c')");

    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    Table t;
    t.name = tbl;
    t.schema = "public";

    EXPECT_EQ(schema->getRowCount(t), 3);
    EXPECT_EQ(schema->getRowCount(t, "id > 1"), 2);

    auto names = schema->getColumnNames(t);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "id");
    EXPECT_EQ(names[1], "name");

    auto rows = schema->getTableData(t, 2, 0, "", "id ASC");
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0], "1");
    EXPECT_EQ(rows[1][0], "2");

    auto more = schema->getTableData(t, 10, 2, "", "id ASC");
    ASSERT_EQ(more.size(), 1u);
    EXPECT_EQ(more[0][0], "3");

    auto filtered = schema->getTableData(t, 10, 0, "name = 'b'");
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0][1], "b");

    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(Postgres, CreateTableViaApiAndDrop) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_pg_create";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);

    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    Table t;
    t.name = tbl;
    t.schema = "public";
    Column id;
    id.name = "id";
    id.type = "INTEGER";
    id.isPrimaryKey = true;
    Column name;
    name.name = "name";
    name.type = "TEXT";
    name.isNotNull = true;
    t.columns = {id, name};
    auto [ok, err] = schema->createTable(t);
    ASSERT_TRUE(ok) << err;

    auto desc = schema->describeTable(tbl);
    EXPECT_EQ(desc.columns.size(), 2u);

    auto [okd, errd] = schema->dropTable(tbl);
    ASSERT_TRUE(okd) << errd;
}

TEST(Postgres, RenameTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string oldT = "dearsql_lib_pg_old";
    const std::string newT = "dearsql_lib_pg_new";
    conn->database()->execute("DROP TABLE IF EXISTS " + oldT);
    conn->database()->execute("DROP TABLE IF EXISTS " + newT);
    conn->database()->execute("CREATE TABLE " + oldT + " (id INT PRIMARY KEY)");

    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    auto [ok, err] = schema->renameTable(oldT, newT);
    ASSERT_TRUE(ok) << err;

    bool foundNew = false;
    for (const auto& t : schema->tables())
        if (t.name == newT)
            foundNew = true;
    EXPECT_TRUE(foundNew);

    conn->database()->execute("DROP TABLE " + newT);
}

TEST(Postgres, TruncateTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_pg_trunc";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY)");
    conn->database()->execute("INSERT INTO " + tbl + " VALUES (1),(2),(3)");

    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    Table t;
    t.name = tbl;
    t.schema = "public";
    EXPECT_EQ(schema->getRowCount(t), 3);
    auto [ok, err] = schema->truncateTable(tbl);
    ASSERT_TRUE(ok) << err;
    EXPECT_EQ(schema->getRowCount(t), 0);

    conn->database()->execute("DROP TABLE " + tbl);
}

TEST(Postgres, DropView) {
    OPEN_OR_SKIP(conn, st);
    conn->database()->execute("CREATE OR REPLACE VIEW dearsql_lib_pg_v AS SELECT 1 AS one");
    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    auto [ok, err] = schema->dropView("dearsql_lib_pg_v");
    ASSERT_TRUE(ok) << err;
}

TEST(Postgres, MaterializedView) {
    OPEN_OR_SKIP(conn, st);
    conn->database()->execute("DROP MATERIALIZED VIEW IF EXISTS dearsql_lib_pg_mv");
    auto r = conn->database()->execute(
        "CREATE MATERIALIZED VIEW dearsql_lib_pg_mv AS SELECT 1 AS one");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    bool found = false;
    for (const auto& mv : schema->materializedViews())
        if (mv.name == "dearsql_lib_pg_mv")
            found = true;
    EXPECT_TRUE(found);
    conn->database()->execute("DROP MATERIALIZED VIEW dearsql_lib_pg_mv");
}

TEST(Postgres, Sequences) {
    OPEN_OR_SKIP(conn, st);
    conn->database()->execute("DROP SEQUENCE IF EXISTS dearsql_lib_pg_seq");
    auto r = conn->database()->execute("CREATE SEQUENCE dearsql_lib_pg_seq START 100");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    auto seqs = schema->sequences();
    bool found = false;
    for (const auto& s : seqs)
        if (s == "dearsql_lib_pg_seq")
            found = true;
    EXPECT_TRUE(found);
    conn->database()->execute("DROP SEQUENCE dearsql_lib_pg_seq");
}

TEST(Postgres, Routines) {
    OPEN_OR_SKIP(conn, st);
    conn->database()->execute("DROP FUNCTION IF EXISTS dearsql_lib_pg_fn(int)");
    auto r = conn->database()->execute(
        "CREATE FUNCTION dearsql_lib_pg_fn(x int) RETURNS int LANGUAGE sql AS $$ SELECT x+1 $$");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    bool found = false;
    for (const auto& fn : schema->routines())
        if (fn.name == "dearsql_lib_pg_fn")
            found = true;
    EXPECT_TRUE(found);
    conn->database()->execute("DROP FUNCTION dearsql_lib_pg_fn(int)");
}

TEST(Postgres, CreateAndDropDatabase) {
    OPEN_OR_SKIP(conn, st);
    const std::string tmp = "dearsql_lib_pg_tmpdb";
    conn->dropDatabase(tmp); // best-effort cleanup
    CreateDatabaseOptions opts;
    opts.name = tmp;
    auto [ok, err] = conn->createDatabase(opts);
    ASSERT_TRUE(ok) << err;

    bool found = false;
    for (auto& d : conn->databases())
        if (d && d->name() == tmp)
            found = true;
    EXPECT_TRUE(found);

    auto [okd, errd] = conn->dropDatabase(tmp);
    ASSERT_TRUE(okd) << errd;
}
