#include "dearsql/dearsql.hpp"
#include <algorithm>
#include <gtest/gtest.h>

using namespace dearsql;

namespace {

ConnectionInfo memInfo(const std::string& name = "test") {
    ConnectionInfo info;
    info.type = DatabaseType::SQLITE;
    info.name = name;
    info.path = ":memory:";
    return info;
}

ConnectionPtr openMem(const std::string& name = "test") {
    auto conn = makeConnection(memInfo(name));
    EXPECT_TRUE(conn);
    auto [ok, err] = conn->open();
    EXPECT_TRUE(ok) << err;
    return conn;
}

} // namespace

TEST(SQLite, OpenAndClose) {
    auto conn = openMem();
    EXPECT_TRUE(conn->isOpen());
    EXPECT_EQ(conn->type(), DatabaseType::SQLITE);
    conn->close();
    EXPECT_FALSE(conn->isOpen());
}

TEST(SQLite, FactoryReturnsConnectionForSqliteType) {
    auto conn = makeConnection(memInfo());
    ASSERT_TRUE(conn);
    EXPECT_EQ(conn->type(), DatabaseType::SQLITE);
}

TEST(SQLite, ListsSingleDatabase) {
    auto conn = openMem("memdb");
    auto dbs = conn->databases();
    ASSERT_EQ(dbs.size(), 1u);
    ASSERT_TRUE(dbs.front());
    EXPECT_EQ(dbs.front()->type(), DatabaseType::SQLITE);
}

TEST(SQLite, DatabaseHandleHasNoSchemaLayer) {
    auto conn = openMem();
    auto db = conn->database();
    ASSERT_TRUE(db);
    EXPECT_EQ(db->type(), DatabaseType::SQLITE);
    // SQLite has no schemas — schemas() returns empty.
    EXPECT_TRUE(db->schemas().empty());
}

TEST(SQLite, ExecuteCreateAndInsertAndSelect) {
    auto conn = openMem();
    auto r = conn->database()->execute(
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL); "
        "INSERT INTO users (id, name) VALUES (1, 'a'), (2, 'b'); "
        "SELECT id, name FROM users ORDER BY id;");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    ASSERT_EQ(r.size(), 3u);

    const auto& sel = r[2];
    ASSERT_EQ(sel.columnNames.size(), 2u);
    EXPECT_EQ(sel.columnNames[0], "id");
    EXPECT_EQ(sel.columnNames[1], "name");
    ASSERT_EQ(sel.tableData.size(), 2u);
    EXPECT_EQ(sel.tableData[0][0], "1");
    EXPECT_EQ(sel.tableData[0][1], "a");
    EXPECT_EQ(sel.tableData[1][0], "2");
    EXPECT_EQ(sel.tableData[1][1], "b");
}

TEST(SQLite, NullSentinelInResult) {
    auto conn = openMem();
    auto r = conn->database()->execute("SELECT NULL AS v");
    ASSERT_TRUE(r.success()) << r.errorMessage();
    ASSERT_EQ(r.size(), 1u);
    ASSERT_EQ(r[0].tableData.size(), 1u);
    EXPECT_TRUE(isNullSentinel(r[0].tableData[0][0]));
}

TEST(SQLite, ListTablesAndViewsAndColumns) {
    auto conn = openMem();
    conn->database()->execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL);");
    conn->database()->execute("CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, "
                  "FOREIGN KEY(user_id) REFERENCES users(id));");
    conn->database()->execute("CREATE VIEW v_users AS SELECT id FROM users;");

    auto db = conn->database();
    auto tables = db->tables();
    ASSERT_EQ(tables.size(), 2u);

    auto users =
        std::find_if(tables.begin(), tables.end(), [](const Table& t) { return t.name == "users"; });
    ASSERT_NE(users, tables.end());
    EXPECT_EQ(users->columns.size(), 2u);
    EXPECT_EQ(users->columns[0].name, "id");
    EXPECT_TRUE(users->columns[0].isPrimaryKey);
    EXPECT_TRUE(users->columns[1].isNotNull);

    auto orders = std::find_if(tables.begin(), tables.end(),
                               [](const Table& t) { return t.name == "orders"; });
    ASSERT_NE(orders, tables.end());
    ASSERT_EQ(orders->foreignKeys.size(), 1u);
    EXPECT_EQ(orders->foreignKeys[0].sourceColumn, "user_id");
    EXPECT_EQ(orders->foreignKeys[0].targetTable, "users");
    EXPECT_EQ(orders->foreignKeys[0].targetColumn, "id");
    EXPECT_FALSE(orders->foreignKeysByColumn.empty());
    EXPECT_FALSE(users->incomingForeignKeys.empty());

    auto views = db->views();
    ASSERT_EQ(views.size(), 1u);
    EXPECT_EQ(views[0].name, "v_users");
}

TEST(SQLite, GetTableDataAndRowCount) {
    auto conn = openMem();
    conn->database()->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);");
    conn->database()->execute("INSERT INTO t (id,name) VALUES (1,'a'),(2,'b'),(3,'c');");

    auto schema = conn->database();
    Table t;
    t.name = "t";

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
}

TEST(SQLite, CreateTableViaApi) {
    auto conn = openMem();
    auto schema = conn->database();

    Table t;
    t.name = "items";
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

    auto tables = schema->tables();
    auto it = std::find_if(tables.begin(), tables.end(),
                           [](const Table& x) { return x.name == "items"; });
    ASSERT_NE(it, tables.end());
    EXPECT_EQ(it->columns.size(), 2u);
}

TEST(SQLite, DescribeTable) {
    auto conn = openMem();
    conn->database()->execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT NOT NULL);");
    auto schema = conn->database();
    auto t = schema->describeTable("t");
    ASSERT_EQ(t.columns.size(), 2u);
    EXPECT_EQ(t.columns[0].name, "id");
    EXPECT_TRUE(t.columns[0].isPrimaryKey);
    EXPECT_TRUE(t.columns[1].isNotNull);
}

TEST(SQLite, RenameAndDropTable) {
    auto conn = openMem();
    conn->database()->execute("CREATE TABLE old_name (id INTEGER PRIMARY KEY);");
    auto schema = conn->database();
    {
        auto [ok, err] = schema->renameTable("old_name", "new_name");
        ASSERT_TRUE(ok) << err;
    }
    auto tables = schema->tables();
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables[0].name, "new_name");

    {
        auto [ok, err] = schema->dropTable("new_name");
        ASSERT_TRUE(ok) << err;
    }
    EXPECT_TRUE(schema->tables().empty());
}

TEST(SQLite, RowAndColumnMutationHelpers) {
    auto conn = openMem();
    auto db = conn->database();
    ASSERT_TRUE(db);

    Table t;
    t.name = "items";
    Column id;
    id.name = "id";
    id.type = "INTEGER";
    id.isPrimaryKey = true;
    t.columns = {id};
    auto [created, createErr] = db->createTable(t);
    ASSERT_TRUE(created) << createErr;

    Column name;
    name.name = "name";
    name.type = "TEXT";
    auto [added, addErr] = db->addColumn(t, name);
    ASSERT_TRUE(added) << addErr;

    auto [inserted, insertErr] = db->insertRow(t, {"id", "name"}, {"1", "'old'"});
    ASSERT_TRUE(inserted) << insertErr;

    auto [updated, updateErr] = db->updateRow(t, {{"name", "'new'"}}, "id = 1");
    ASSERT_TRUE(updated) << updateErr;

    auto rows = db->getTableData(t, 10, 0, "", "id ASC");
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][1], "new");

    auto [renamed, renameErr] = db->renameColumn(t, "name", "label");
    ASSERT_TRUE(renamed) << renameErr;
    auto cols = db->getColumnNames(t);
    EXPECT_NE(std::find(cols.begin(), cols.end(), "label"), cols.end());

    auto [deleted, deleteErr] = db->deleteRow(t, "id = 1");
    ASSERT_TRUE(deleted) << deleteErr;
    EXPECT_EQ(db->getRowCount(t), 0);
}

TEST(SQLite, ExecuteErrorReturnsFailure) {
    auto conn = openMem();
    auto r = conn->database()->execute("SELECT * FROM does_not_exist;");
    EXPECT_FALSE(r.success());
    EXPECT_FALSE(r.errorMessage().empty());
}

TEST(SQLite, ListSequencesAfterAutoincrement) {
    auto conn = openMem();
    conn->database()->execute("CREATE TABLE t (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT);");
    conn->database()->execute("INSERT INTO t (name) VALUES ('a');");
    auto schema = conn->database();
    auto seqs = schema->sequences();
    ASSERT_EQ(seqs.size(), 1u);
    EXPECT_EQ(seqs[0], "t");
}
