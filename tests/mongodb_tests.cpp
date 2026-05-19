#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_MONGODB_HOST", "DEARSQL_TEST_MONGODB_PORT",
                        "DEARSQL_TEST_MONGODB_DB", "DEARSQL_TEST_MONGODB_USER",
                        "DEARSQL_TEST_MONGODB_PASSWORD", "DEARSQL_TEST_MONGODB_NAME", 27017);
    if (c && c->name.empty())
        c->name = "MongoDBDocker";
    if (c && c->database.empty())
        c->database = "test";
    return c;
}

#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_MONGODB_HOST");                                     \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::MONGODB);                              \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second
} // namespace

TEST(MongoDB, OpenAndListDatabases) {
    OPEN_OR_SKIP(conn, st);
    ASSERT_TRUE(conn->isOpen());
    EXPECT_FALSE(conn->databases().empty());
}

TEST(MongoDB, NoSchemaLayer) {
    OPEN_OR_SKIP(conn, st);
    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    EXPECT_TRUE(db->schemas().empty());
}

TEST(MongoDB, ListCollectionsAfterCreate) {
    OPEN_OR_SKIP(conn, st);
    const std::string coll = "dearsql_lib_mongo_coll";
    // mongo "execute" takes a JSON command; the app's MongoDB backend
    // accepts {"insert":"coll","documents":[{...}]} style.
    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    db->dropTable(coll); // best-effort
    auto r = db->execute(R"({"insert":")" + coll + R"(","documents":[{"k":1}]})");
    ASSERT_TRUE(r.success()) << r.errorMessage();

    bool found = false;
    for (const auto& t : db->tables())
        if (t.name == coll)
            found = true;
    EXPECT_TRUE(found);
    db->dropTable(coll);
}

TEST(MongoDB, InsertAndGetTableData) {
    OPEN_OR_SKIP(conn, st);
    const std::string coll = "dearsql_lib_mongo_data";
    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    db->dropTable(coll);

    auto r = db->execute(R"({"insert":")" + coll +
                         R"(","documents":[{"k":1,"v":"a"},{"k":2,"v":"b"},{"k":3,"v":"c"}]})");
    ASSERT_TRUE(r.success()) << r.errorMessage();

    Table t;
    t.name = coll;
    EXPECT_EQ(db->getRowCount(t), 3);

    auto rows = db->getTableData(t, 10, 0);
    EXPECT_EQ(rows.size(), 3u);

    db->dropTable(coll);
}

TEST(MongoDB, DropCollection) {
    OPEN_OR_SKIP(conn, st);
    const std::string coll = "dearsql_lib_mongo_drop";
    auto db = conn->database(c->database);
    ASSERT_TRUE(db);
    db->execute(R"({"insert":")" + coll + R"(","documents":[{"k":1}]})");
    auto [ok, err] = db->dropTable(coll);
    ASSERT_TRUE(ok) << err;
    bool found = false;
    for (const auto& t : db->tables())
        if (t.name == coll)
            found = true;
    EXPECT_FALSE(found);
}

TEST(MongoDB, DropDatabase) {
    OPEN_OR_SKIP(conn, st);
    const std::string tmp = "dearsql_lib_mongo_tmpdb";
    auto tmpDb = conn->database(tmp);
    ASSERT_TRUE(tmpDb);
    // create something so the db actually materializes
    tmpDb->execute(R"({"insert":"x","documents":[{"k":1}]})");
    auto [ok, err] = conn->dropDatabase(tmp);
    ASSERT_TRUE(ok) << err;
}

TEST(MongoDB, ExecuteErrorReturnsFailure) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("this is not valid json");
    EXPECT_FALSE(r.success());
    EXPECT_FALSE(r.errorMessage().empty());
}
