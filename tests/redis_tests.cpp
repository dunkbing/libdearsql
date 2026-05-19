#include "dearsql/backends/redis_connection.hpp"
#include "test_helpers.hpp"

using namespace dearsql;
using namespace dearsql::testing;

namespace {
std::optional<BackendConfig> cfg() {
    auto c = loadConfig("DEARSQL_TEST_REDIS_HOST", "DEARSQL_TEST_REDIS_PORT", nullptr, nullptr,
                        nullptr, "DEARSQL_TEST_REDIS_NAME", 6379);
    if (c && c->name.empty())
        c->name = "RedisDocker";
    return c;
}

RedisConnection* asRedis(ConnectionPtr conn) {
    return dynamic_cast<RedisConnection*>(conn.get());
}

#define OPEN_OR_SKIP(conn_var, status_var)                                                         \
    auto c = cfg();                                                                                \
    DEARSQL_SKIP_IF_NO_CONFIG(c, "DEARSQL_TEST_REDIS_HOST");                                       \
    auto [conn_var, status_var] = tryOpen(*c, DatabaseType::REDIS);                                \
    DEARSQL_SKIP_IF_UNIMPLEMENTED(status_var);                                                     \
    ASSERT_TRUE(status_var.first) << status_var.second
} // namespace

TEST(Redis, OpenAndPing) {
    OPEN_OR_SKIP(conn, st);
    ASSERT_TRUE(conn->isOpen());
    auto r = conn->database()->execute("PING");
    EXPECT_TRUE(r.success()) << r.errorMessage();
}

TEST(Redis, ListLogicalDatabases) {
    OPEN_OR_SKIP(conn, st);
    auto dbs = conn->databases();
    EXPECT_FALSE(dbs.empty()); // default config = 16 logical DBs
}

TEST(Redis, SetGetDelString) {
    OPEN_OR_SKIP(conn, st);
    const std::string key = "dearsql:lib:test:str";
    ASSERT_TRUE(conn->database()->execute("SET " + key + " 42").success());
    auto* r = asRedis(conn);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->getKeyValue(key), "42");
    EXPECT_EQ(r->getKeyType(key), "string");
    conn->database()->execute("DEL " + key);
}

TEST(Redis, KeyTTL) {
    OPEN_OR_SKIP(conn, st);
    const std::string key = "dearsql:lib:test:ttl";
    conn->database()->execute("SET " + key + " v EX 60");
    auto* r = asRedis(conn);
    ASSERT_TRUE(r);
    auto ttl = r->getKeyTTL(key);
    EXPECT_GT(ttl, 0);
    EXPECT_LE(ttl, 60);
    conn->database()->execute("DEL " + key);
}

TEST(Redis, KeyTypes) {
    OPEN_OR_SKIP(conn, st);
    conn->database()->execute("RPUSH dearsql:lib:test:list a b c");
    conn->database()->execute("HSET dearsql:lib:test:hash f1 v1");
    conn->database()->execute("SADD dearsql:lib:test:set m1 m2");
    auto* r = asRedis(conn);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->getKeyType("dearsql:lib:test:list"), "list");
    EXPECT_EQ(r->getKeyType("dearsql:lib:test:hash"), "hash");
    EXPECT_EQ(r->getKeyType("dearsql:lib:test:set"), "set");
    conn->database()->execute("DEL dearsql:lib:test:list dearsql:lib:test:hash dearsql:lib:test:set");
}

TEST(Redis, ScanKeysByPattern) {
    OPEN_OR_SKIP(conn, st);
    conn->database()->execute("MSET dearsql:lib:scan:1 a dearsql:lib:scan:2 b dearsql:lib:scan:3 c");
    auto* r = asRedis(conn);
    ASSERT_TRUE(r);
    auto keys = r->getKeys("dearsql:lib:scan:*", 100);
    EXPECT_GE(keys.size(), 3u);
    conn->database()->execute("DEL dearsql:lib:scan:1 dearsql:lib:scan:2 dearsql:lib:scan:3");
}

TEST(Redis, SelectDatabase) {
    OPEN_OR_SKIP(conn, st);
    auto* r = asRedis(conn);
    ASSERT_TRUE(r);
    EXPECT_TRUE(r->selectDatabase(1));
    conn->database()->execute("SET dearsql:lib:db1key v");
    EXPECT_EQ(r->getKeyValue("dearsql:lib:db1key"), "v");
    conn->database()->execute("DEL dearsql:lib:db1key");
    EXPECT_TRUE(r->selectDatabase(0));
}

TEST(Redis, ExecuteErrorReturnsFailure) {
    OPEN_OR_SKIP(conn, st);
    auto r = conn->database()->execute("GET"); // missing args
    EXPECT_FALSE(r.success());
}
