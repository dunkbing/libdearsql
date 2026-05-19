#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_CASSANDRA_HOST", "DEARSQL_TEST_CASSANDRA_PORT", nullptr,
                        nullptr, nullptr, "DEARSQL_TEST_CASSANDRA_NAME", 9042);
    if (c && c->name.empty())
        c->name = "CassandraDocker";
    return c;
}

#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_CASSANDRA_HOST");                                   \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::CASSANDRA);                            \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second

const std::string TEST_KS = "dearsql_lib_ks";

void resetKeyspace(ConnectionPtr conn) {
    conn->database()->execute("DROP KEYSPACE IF EXISTS " + TEST_KS);
    conn->database()->execute(
        "CREATE KEYSPACE " + TEST_KS +
        " WITH replication = {'class':'SimpleStrategy','replication_factor':1}");
}
} // namespace

TEST(Cassandra, OpenAndListKeyspaces) {
    OPEN_OR_SKIP(conn, st);
    ASSERT_TRUE(conn->isOpen());
    auto dbs = conn->databases();
    EXPECT_FALSE(dbs.empty()); // system_schema is always there
    bool foundSystem = false;
    for (auto& d : dbs)
        if (d && d->name() == "system_schema")
            foundSystem = true;
    EXPECT_TRUE(foundSystem);
}

TEST(Cassandra, ExecuteSelectFromSystem) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute(
        "SELECT keyspace_name FROM system_schema.keyspaces LIMIT 1");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    EXPECT_FALSE(r[0].tableData.empty());
}

TEST(Cassandra, ExecuteErrorReturnsFailure) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("SELECT * FROM dearsql_does_not_exist_xyz");
    EXPECT_FALSE(r.success());
}

TEST(Cassandra, CreateAndDropKeyspace) {
    OPEN_OR_SKIP(conn, st);
    const std::string ks = "dearsql_lib_tmp_ks";
    conn->database()->execute("DROP KEYSPACE IF EXISTS " + ks);
    CreateDatabaseOptions opts;
    opts.name = ks;
    auto [ok, err] = conn->createDatabase(opts);
    ASSERT_TRUE(ok) << err;
    auto [okd, errd] = conn->dropDatabase(ks);
    ASSERT_TRUE(okd) << errd;
}

TEST(Cassandra, RoundTripTable) {
    OPEN_OR_SKIP(conn, st);
    resetKeyspace(conn);
    auto db = conn->database(TEST_KS);
    ASSERT_TRUE(db);

    {
        auto r = db->execute("CREATE TABLE t (id int PRIMARY KEY, name text)");
        ASSERT_TRUE(r.success()) << r.errorMessage();
    }
    db->execute("INSERT INTO t (id,name) VALUES (1,'a')");
    db->execute("INSERT INTO t (id,name) VALUES (2,'b')");

    Table t;
    t.name = "t";
    EXPECT_EQ(db->getRowCount(t), 2);

    auto rows = db->getTableData(t, 10, 0);
    EXPECT_EQ(rows.size(), 2u);

    conn->database()->execute("DROP KEYSPACE " + TEST_KS);
}

TEST(Cassandra, DescribeTable) {
    OPEN_OR_SKIP(conn, st);
    resetKeyspace(conn);
    auto db = conn->database(TEST_KS);
    ASSERT_TRUE(db);
    db->execute("CREATE TABLE t (id int PRIMARY KEY, name text, qty int)");
    auto t = db->describeTable("t");
    EXPECT_EQ(t.columns.size(), 3u);
    EXPECT_TRUE(t.columns[0].isPrimaryKey);
    conn->database()->execute("DROP KEYSPACE " + TEST_KS);
}

TEST(Cassandra, ListTables) {
    OPEN_OR_SKIP(conn, st);
    resetKeyspace(conn);
    auto db = conn->database(TEST_KS);
    ASSERT_TRUE(db);
    db->execute("CREATE TABLE t1 (id int PRIMARY KEY)");
    db->execute("CREATE TABLE t2 (id int PRIMARY KEY)");
    auto tables = db->tables();
    EXPECT_GE(tables.size(), 2u);
    conn->database()->execute("DROP KEYSPACE " + TEST_KS);
}

TEST(Cassandra, CreateAndDropTableViaApi) {
    OPEN_OR_SKIP(conn, st);
    resetKeyspace(conn);
    auto db = conn->database(TEST_KS);
    ASSERT_TRUE(db);
    Table t;
    t.name = "tab";
    Column id;
    id.name = "id";
    id.type = "int";
    id.isPrimaryKey = true;
    Column name;
    name.name = "name";
    name.type = "text";
    t.columns = {id, name};
    auto [ok, err] = db->createTable(t);
    ASSERT_TRUE(ok) << err;
    auto [okd, errd] = db->dropTable("tab");
    ASSERT_TRUE(okd) << errd;
    conn->database()->execute("DROP KEYSPACE " + TEST_KS);
}
