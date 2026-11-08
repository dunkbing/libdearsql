#include "dearsql/dearsql.hpp"
#include "dearsql/delete_impact.hpp"
#include <gtest/gtest.h>

using namespace dearsql;

namespace {
    int count(IDatabase& db, const std::string& sql) {
        const auto r = db.execute(sql, 0);
        EXPECT_TRUE(r.success()) << sql;
        return r.success() && !r.empty() && !r[0].tableData.empty()
                   ? std::stoi(r[0].tableData[0][0])
                   : -1;
    }

    const DeleteEffect* find(const std::vector<DeleteEffect>& effects, const std::string& path) {
        for (const auto& e : effects)
            if (e.path == path)
                return &e;
        return nullptr;
    }
} // namespace

// a cascade chain, a set-null key, a blocking key, a self-reference and a composite key; the
// counts the queries give must be what the real delete then does
TEST(DeleteImpact, CountsMatchTheRealDelete) {
    ConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.name = "impact";
    info.path = ":memory:";
    auto conn = makeConnection(info);
    ASSERT_TRUE(conn && conn->open().first);
    auto& db = *conn->database();
    for (const char* sql : {
             "CREATE TABLE customers (id INTEGER PRIMARY KEY, referrer INTEGER REFERENCES "
             "customers(id) ON DELETE CASCADE)",
             "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES "
             "customers(id) ON DELETE CASCADE, region TEXT, UNIQUE (id, region))",
             "CREATE TABLE items (id INTEGER PRIMARY KEY, order_id INTEGER, region TEXT, "
             "FOREIGN KEY (order_id, region) REFERENCES orders(id, region) ON DELETE CASCADE)",
             "CREATE TABLE notes (id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES "
             "customers(id) ON DELETE SET NULL)",
             "CREATE TABLE invoices (id INTEGER PRIMARY KEY, customer_id INTEGER REFERENCES "
             "customers(id))",
             "INSERT INTO customers VALUES (1, NULL), (2, 1), (3, NULL)",
             "INSERT INTO orders VALUES (10, 1, 'eu'), (11, 2, 'eu'), (12, 3, 'us')",
             "INSERT INTO items VALUES (100, 10, 'eu'), (101, 10, 'eu'), (102, 11, 'eu'), "
             "(103, 12, 'us')",
             "INSERT INTO notes VALUES (1, 1), (2, 3)",
         }) {
        ASSERT_TRUE(db.execute(sql, 0).success()) << sql;
    }
    auto tables = db.tables();
    const auto root = std::ranges::find(tables, std::string("customers"), &Table::name);
    ASSERT_NE(root, tables.end());

    const auto effects = deleteEffects(DatabaseType::SQLITE, tables, *root, "\"id\" = 1");
    const auto* orders = find(effects, "customers > orders");
    const auto* items = find(effects, "customers > orders > items");
    const auto* self = find(effects, "customers > customers");
    const auto* selfOrders = find(effects, "customers > customers > orders");
    const auto* notes = find(effects, "customers > notes");
    const auto* invoices = find(effects, "customers > invoices");
    ASSERT_TRUE(orders && items && self && selfOrders && notes && invoices);
    EXPECT_EQ(orders->action, "CASCADE");
    EXPECT_EQ(items->columns, "order_id, region");
    EXPECT_EQ(items->depth, 2);
    EXPECT_EQ(notes->action, "SET NULL");
    EXPECT_EQ(invoices->action, "NO ACTION");

    EXPECT_EQ(count(db, orders->countSql), 1);     // order 10
    EXPECT_EQ(count(db, items->countSql), 2);      // its two items
    EXPECT_EQ(count(db, self->countSql), 1);       // customer 2, referred by 1
    EXPECT_EQ(count(db, selfOrders->countSql), 1); // customer 2's order
    EXPECT_EQ(count(db, notes->countSql), 1);
    EXPECT_EQ(count(db, invoices->countSql), 0); // nothing blocks it

    ASSERT_TRUE(db.execute("DELETE FROM customers WHERE id = 1", 0).success());
    EXPECT_EQ(count(db, "SELECT COUNT(*) FROM customers"), 1); // 1 and 2 gone
    EXPECT_EQ(count(db, "SELECT COUNT(*) FROM orders"), 1);    // 10 and 11 gone
    EXPECT_EQ(count(db, "SELECT COUNT(*) FROM items"), 1);     // only 103 left
    EXPECT_EQ(count(db, "SELECT COUNT(*) FROM notes WHERE customer_id IS NULL"), 1);
}

// no rows, no queries; and a cascade cycle stops at maxDepth
TEST(DeleteImpact, Limits) {
    Table t{.name = "nodes"};
    t.foreignKeys.push_back({"fk_parent", "parent_id", "nodes", "id", "CASCADE", ""});
    EXPECT_TRUE(deleteEffects(DatabaseType::POSTGRESQL, {t}, t, "").empty());
    const auto effects = deleteEffects(DatabaseType::ORACLE, {t}, t, "\"ID\" = 1", 2);
    ASSERT_EQ(effects.size(), 2u);
    EXPECT_EQ(effects[1].path, "nodes > nodes > nodes");
    EXPECT_EQ(effects[0].countSql.find(" AS "), std::string::npos); // oracle: no AS on aliases
}
