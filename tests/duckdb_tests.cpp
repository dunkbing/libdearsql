#include "dearsql/dearsql.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace dearsql;

namespace {

ConnectionPtr openDuck(const std::string& path = ":memory:") {
    ConnectionInfo info;
    info.type = DatabaseType::DUCKDB;
    info.name = "duck";
    info.path = path;
    auto conn = makeConnection(info);
    EXPECT_TRUE(conn);
    auto [ok, err] = conn->open();
    EXPECT_TRUE(ok) << err;
    return conn;
}

} // namespace

TEST(DuckDB, CatalogHasKeysIndexesAndForeignKeys) {
    auto db = openDuck()->database();
    auto r = db->execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email VARCHAR NOT NULL DEFAULT 'x');"
                         "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER REFERENCES users(id));"
                         "CREATE VIEW big_orders AS SELECT * FROM orders WHERE id > 10;",
                         0);
    ASSERT_TRUE(r.success()) << r.errorMessage();

    auto tables = db->tables();
    ASSERT_EQ(tables.size(), 2u);
    auto orders = std::ranges::find(tables, "orders", &Table::name);
    ASSERT_NE(orders, tables.end());
    ASSERT_EQ(orders->foreignKeys.size(), 1u);
    EXPECT_EQ(orders->foreignKeys[0].targetTable, "users");
    auto users = std::ranges::find(tables, "users", &Table::name);
    EXPECT_EQ(users->incomingForeignKeys.size(), 1u);
    EXPECT_TRUE(users->columns[1].isNotNull);
    EXPECT_FALSE(users->columns[1].defaultValue.empty());

    auto views = db->views();
    ASSERT_EQ(views.size(), 1u);
    EXPECT_FALSE(views[0].definition.empty());
    EXPECT_EQ(views[0].columns.size(), 2u);
}

TEST(DuckDB, ParallelHandlesShareOneDatabase) {
    auto conn = openDuck();
    ASSERT_TRUE(conn->database()->execute("CREATE TABLE t (v INTEGER); INSERT INTO t VALUES (1), (2)", 0).success());
    auto other = conn->openDatabase();
    ASSERT_TRUE(other);
    EXPECT_EQ(other->getRowCount(Table{.name = "t"}), 2);
}

TEST(DuckDB, CsvOpensAsTable) {
    const auto path = std::filesystem::temp_directory_path() / "libdearsql_people.csv";
    std::ofstream(path) << "name,age\nada,36\nlinus,28\n";
    auto db = openDuck(path.string())->database();
    auto tables = db->tables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0].name, "libdearsql_people");
    EXPECT_EQ(db->getRowCount(tables[0]), 2);
    std::filesystem::remove(path);
}

TEST(DuckDB, CatalogErrorsThrow) {
    auto db = openDuck()->database();
    EXPECT_THROW(db->getTableData(Table{.name = "missing"}, 10, 0), Error);
}
