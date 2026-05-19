#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_REDSHIFT_HOST", "DEARSQL_TEST_REDSHIFT_PORT",
                        "DEARSQL_TEST_REDSHIFT_DB", "DEARSQL_TEST_REDSHIFT_USER",
                        "DEARSQL_TEST_REDSHIFT_PASSWORD", "DEARSQL_TEST_REDSHIFT_NAME", 5439);
    if (c && c->name.empty())
        c->name = "RedshiftDocker";
    if (c && c->database.empty())
        c->database = "dev";
    return c;
}

DatabasePtr publicSchema(ConnectionPtr conn) {
    for (auto& s : conn->database()->schemas())
        if (s && s->name() == "public")
            return s;
    return nullptr;
}

// Redshift shares the Postgres backend in libdearsql; these tests verify the
// shared path still works against a postgres-image-as-redshift stand-in.
#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_REDSHIFT_HOST");                                    \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::REDSHIFT);                             \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second
} // namespace

TEST(Redshift, OpenAndExecute) {
    OPEN_OR_SKIP(conn, st);
    ASSERT_TRUE(conn->isOpen());
    auto r = conn->database()->execute("SELECT 1 AS one");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    EXPECT_EQ(r[0].tableData[0][0], "1");
}

TEST(Redshift, ListDatabases) {
    OPEN_OR_SKIP(conn, st);
    EXPECT_FALSE(conn->databases().empty());
}

TEST(Redshift, RoundTripTable) {
    OPEN_OR_SKIP(conn, st);
    const std::string tbl = "dearsql_lib_rs_round";
    conn->database()->execute("DROP TABLE IF EXISTS " + tbl);
    conn->database()->execute("CREATE TABLE " + tbl + " (id INT PRIMARY KEY, name TEXT)");
    conn->database()->execute("INSERT INTO " + tbl + " VALUES (1,'a'),(2,'b')");
    auto schema = publicSchema(conn);
    ASSERT_TRUE(schema);
    Table t;
    t.name = tbl;
    t.schema = "public";
    EXPECT_EQ(schema->getRowCount(t), 2);
    conn->database()->execute("DROP TABLE " + tbl);
}
