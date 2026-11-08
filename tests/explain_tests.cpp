#include <dearsql/explain.hpp>
#include <dearsql/findings.hpp>
#include <gtest/gtest.h>

using namespace dearsql;

namespace {
    StatementResult oneColumn(const std::string& text) {
        StatementResult r;
        r.columnNames = {"QUERY PLAN"};
        r.tableData = {{text}};
        return r;
    }
} // namespace

TEST(Explain, StatementPerDialect) {
    EXPECT_EQ(explainSql(DatabaseType::POSTGRESQL, " select 1; ", false),
              "EXPLAIN (FORMAT JSON) select 1");
    EXPECT_EQ(explainSql(DatabaseType::MYSQL, "select 1", true), "EXPLAIN ANALYZE select 1");
    EXPECT_EQ(explainSql(DatabaseType::SQLITE, "select 1", false), "EXPLAIN QUERY PLAN select 1");
    EXPECT_FALSE(explainSql(DatabaseType::SQLITE, "select 1", true));
    EXPECT_FALSE(explainSql(DatabaseType::REDIS, "get k", false));
    EXPECT_FALSE(explainSql(DatabaseType::POSTGRESQL, " ; ", false));
}

TEST(Explain, PostgresJsonAnalyze) {
    const auto plan = parsePlan(DatabaseType::POSTGRESQL, oneColumn(R"json([{
      "Plan": {"Node Type": "Hash Join", "Join Type": "Left", "Total Cost": 20.5,
               "Plan Rows": 100, "Actual Rows": 90, "Actual Loops": 1,
               "Actual Total Time": 5.0, "Hash Cond": "(a.id = b.a_id)",
        "Plans": [
          {"Node Type": "Seq Scan", "Relation Name": "a", "Alias": "a", "Plan Rows": 100,
           "Actual Rows": 90, "Actual Loops": 1, "Actual Total Time": 1.0},
          {"Node Type": "Index Scan", "Relation Name": "b", "Alias": "bb", "Index Name": "b_pk",
           "Plan Rows": 1, "Actual Rows": 2, "Actual Loops": 3, "Actual Total Time": 0.5}
        ]},
      "Planning Time": 0.2, "Execution Time": 5.1}])json"),
                                true);
    ASSERT_TRUE(plan);
    EXPECT_TRUE(plan->analyzed);
    EXPECT_EQ(plan->executionTimeMs, 5.1);
    const PlanNode& root = plan->root;
    EXPECT_EQ(root.operation, "Hash Left Join");
    EXPECT_EQ(root.cost, 20.5);
    ASSERT_EQ(root.properties.size(), 1u);
    EXPECT_EQ(root.properties[0].first, "Hash Cond");
    ASSERT_EQ(root.children.size(), 2u);
    EXPECT_EQ(root.children[1].detail, "on b bb using b_pk");
    // per-loop time x loops, and self = total - children
    EXPECT_DOUBLE_EQ(*root.children[1].totalTimeMs, 1.5);
    EXPECT_DOUBLE_EQ(*root.selfTimeMs, 2.5);
}

TEST(Explain, PostgresGarbageIsNullopt) {
    EXPECT_FALSE(parsePlan(DatabaseType::POSTGRESQL, oneColumn("Seq Scan on t"), false));
}

TEST(Explain, MysqlTreeAnalyze) {
    const auto plan = parsePlan(
        DatabaseType::MYSQL,
        oneColumn("-> Limit: 3 row(s)  (actual time=21.3..21.3 rows=3 loops=1)\n"
                  "    -> Nested loop inner join  (cost=0.7 rows=1) (actual time=0.1..20 rows=3 "
                  "loops=1)\n"
                  "        -> Index lookup on u using PRIMARY (Host='x')  (cost=0.35 rows=1) "
                  "(actual time=575e-6..791e-6 rows=5 loops=362)\n"
                  "        -> Hash\n"
                  "            -> Table scan on d  (cost=0.35 rows=1) (never executed)\n"),
        true);
    ASSERT_TRUE(plan);
    const PlanNode& root = plan->root;
    EXPECT_EQ(root.operation, "Limit: 3 row(s)");
    ASSERT_EQ(root.children.size(), 1u);
    const PlanNode& join = root.children[0];
    EXPECT_EQ(join.operation, "Nested loop inner join");
    EXPECT_EQ(join.estimatedRows, 1.0);
    ASSERT_EQ(join.children.size(), 2u);
    EXPECT_EQ(join.children[0].operation, "Index lookup on u using PRIMARY (Host='x')");
    EXPECT_EQ(join.children[0].loops, 362.0);
    EXPECT_NEAR(*join.children[0].totalTimeMs, 791e-6 * 362, 1e-9);
    ASSERT_EQ(join.children[1].children.size(), 1u);
    EXPECT_EQ(join.children[1].children[0].loops, 0.0);
    EXPECT_NEAR(*root.selfTimeMs, 1.3, 1e-9);
}

TEST(Explain, SqliteQueryPlan) {
    StatementResult r;
    r.columnNames = {"id", "parent", "notused", "detail"};
    r.tableData = {{"2", "0", "0", "SCAN a"},
                   {"5", "0", "0", "SEARCH b USING INDEX b_a (a_id=?)"},
                   {"9", "0", "0", "USE TEMP B-TREE FOR ORDER BY"},
                   {"12", "9", "0", "child of temp"}};
    const auto plan = parsePlan(DatabaseType::SQLITE, r, false);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->root.operation, "QUERY PLAN");
    ASSERT_EQ(plan->root.children.size(), 3u);
    EXPECT_EQ(plan->root.children[1].operation, "SEARCH b USING INDEX b_a (a_id=?)");
    ASSERT_EQ(plan->root.children[2].children.size(), 1u);
}

// parallel workers run their loops concurrently: per-loop time is the wall time
TEST(Explain, PostgresParallelTimeIsNotMultiplied) {
    const auto plan = parsePlan(DatabaseType::POSTGRESQL, oneColumn(R"json([{"Plan": {
        "Node Type": "Gather", "Actual Total Time": 12.0, "Actual Loops": 1,
        "Plans": [{"Node Type": "Seq Scan", "Parallel Aware": true, "Relation Name": "t",
                   "Actual Total Time": 10.0, "Actual Loops": 3}]}}])json"),
                                true);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->root.children[0].operation, "Parallel Seq Scan");
    EXPECT_DOUBLE_EQ(*plan->root.children[0].totalTimeMs, 10.0);
    EXPECT_DOUBLE_EQ(*plan->root.selfTimeMs, 2.0);
}

namespace {
    std::vector<std::string> messages(const std::optional<Plan>& plan) {
        std::vector<std::string> out;
        for (const auto& w : reviewPlan(*plan))
            out.push_back(w.message);
        return out;
    }
    bool mentions(const std::vector<std::string>& ms, std::string_view text) {
        return std::ranges::any_of(ms, [&](const auto& m) { return m.find(text) != m.npos; });
    }
} // namespace

TEST(Explain, ReviewPostgresFilteredScanLoopsSortAndEstimates) {
    const auto plan = parsePlan(DatabaseType::POSTGRESQL, oneColumn(R"json([{
      "Plan": {"Node Type": "Sort", "Plan Rows": 10, "Actual Rows": 5000, "Actual Loops": 1,
               "Sort Method": "external merge", "Sort Space Used": 2048,
        "Plans": [{"Node Type": "Nested Loop", "Plan Rows": 10, "Actual Rows": 5000,
                   "Actual Loops": 1,
          "Plans": [
            {"Node Type": "Seq Scan", "Relation Name": "orders", "Alias": "o",
             "Filter": "(status = 'open')", "Rows Removed by Filter": 99000,
             "Plan Rows": 1000, "Actual Rows": 1000, "Actual Loops": 1},
            {"Node Type": "Seq Scan", "Relation Name": "users", "Alias": "u",
             "Plan Rows": 5, "Actual Rows": 5, "Actual Loops": 1000}
          ]}]}}])json"),
                                true);
    const auto ms = messages(plan);
    EXPECT_TRUE(mentions(ms, "Full scan of orders reads 100000 rows to keep 1000"));
    EXPECT_TRUE(mentions(ms, "users is read in full once per row of the outer side (1000 times)"));
    EXPECT_TRUE(mentions(ms, "spilled to disk (2048 kB)"));
    EXPECT_TRUE(mentions(ms, "Expected 10 rows, got 5000"));
    EXPECT_EQ(ms.size(), 5u) << "sort and join both misestimated";
    EXPECT_EQ(reviewPlan(*plan)[0].node, &plan->root);
}

TEST(Explain, ReviewMysqlFilterAboveTableScan) {
    const auto plan = parsePlan(DatabaseType::MYSQL,
                                oneColumn("-> Filter: (t.a = 3)  (cost=1000 rows=50)\n"
                                          "    -> Table scan on t  (cost=1000 rows=10000)\n"),
                                false);
    const auto ms = messages(plan);
    ASSERT_EQ(ms.size(), 1u);
    EXPECT_TRUE(mentions(ms, "Full scan of t reads 10000 rows to keep 50"));
}

TEST(Explain, ReviewSqliteJoinScanAndAutomaticIndex) {
    StatementResult r;
    r.columnNames = {"id", "parent", "notused", "detail"};
    r.tableData = {{"2", "0", "0", "SCAN a"},
                   {"4", "0", "0", "SCAN b"},
                   {"6", "0", "0", "SEARCH c USING AUTOMATIC COVERING INDEX (k=?)"}};
    const auto ms = messages(parsePlan(DatabaseType::SQLITE, r, false));
    ASSERT_EQ(ms.size(), 2u);
    EXPECT_TRUE(mentions(ms, "b is read in full once per row of the outer side."));
    EXPECT_TRUE(mentions(ms, "temporary index on c"));
}

TEST(Explain, ReviewQuietOnGoodPlans) {
    StatementResult r;
    r.columnNames = {"id", "parent", "notused", "detail"};
    r.tableData = {{"2", "0", "0", "SCAN a"}, {"5", "0", "0", "SEARCH b USING INDEX b_a (a_id=?)"}};
    EXPECT_TRUE(messages(parsePlan(DatabaseType::SQLITE, r, false)).empty());
    // a small filtered scan is fine
    EXPECT_TRUE(messages(parsePlan(DatabaseType::POSTGRESQL, oneColumn(R"json([{"Plan":
        {"Node Type": "Seq Scan", "Relation Name": "t", "Filter": "(a = 1)", "Total Cost": 35,
         "Plan Rows": 3}}])json"),
                                   false))
                    .empty());
}

TEST(Findings, EveryCodeHasADoc) {
    for (const auto* code : {"filtered-full-scan", "repeated-full-scan", "sort-spill", "big-sort",
                             "misestimate", "automatic-index", "unindexed-fk", "redundant-index"})
        EXPECT_NE(findingDoc(code), nullptr) << code;
    EXPECT_EQ(findingDoc("nope"), nullptr);
    // plan warnings carry their code
    StatementResult r;
    r.columnNames = {"id", "parent", "notused", "detail"};
    r.tableData = {{"2", "0", "0", "SCAN a"}, {"4", "0", "0", "SCAN b"}};
    const auto ws = reviewPlan(*parsePlan(DatabaseType::SQLITE, r, false));
    ASSERT_EQ(ws.size(), 1u);
    EXPECT_EQ(ws[0].code, "repeated-full-scan");
}

TEST(Findings, ReviewIndexes) {
    auto col = [](const char* name, bool pk = false) {
        Column c;
        c.name = name;
        c.isPrimaryKey = pk;
        return c;
    };
    Table orders;
    orders.name = "orders";
    orders.schema = "public";
    orders.columns = {col("id", true), col("user_id"), col("shop_id"), col("region"),
                      col("created_at")};
    orders.foreignKeys = {
        {.name = "fk_user", .sourceColumn = "user_id", .targetTable = "users"},
        {.name = "fk_shop", .sourceColumn = "shop_id", .targetTable = "shops"},
        {.name = "fk_shop", .sourceColumn = "region", .targetTable = "shops"},
        {.name = "fk_self", .sourceColumn = "id", .targetTable = "legacy"}};
    orders.indexes = {
        {.name = "orders_user", .columns = {"user_id"}},
        {.name = "orders_user_created", .columns = {"user_id", "created_at"}},
        {.name = "orders_region_shop", .columns = {"region", "shop_id"}},
        {.name = "orders_created", .columns = {"created_at"}},
        {.name = "orders_created_2", .columns = {"created_at"}},
        {.name = "orders_created_u", .columns = {"created_at"}, .isUnique = true}};
    Table items;
    items.name = "items";
    items.columns = {col("id", true), col("order_id")};
    items.foreignKeys = {{.name = "fk_order", .sourceColumn = "order_id", .targetTable = "orders"}};

    const auto fs = reviewIndexes({orders, items});
    std::vector<std::string> got;
    for (const auto& f : fs)
        got.push_back(f.code + " " + f.message);
    // composite fk in another column order and the pk-covered fk are fine
    ASSERT_EQ(fs.size(), 4u) << ::testing::PrintToString(got);
    EXPECT_TRUE(mentions(got, "redundant-index public.orders: index orders_user (user_id) leads "
                              "orders_user_created"));
    // two plain duplicates of a unique index: both go, the unique one stays
    EXPECT_TRUE(mentions(got, "index orders_created (created_at) duplicates orders_created_u"));
    EXPECT_TRUE(mentions(got, "index orders_created_2 (created_at) duplicates orders_created_u"));
    EXPECT_TRUE(mentions(got, "unindexed-fk items (order_id) references orders"));
    EXPECT_EQ(fs.back().suggestion, "CREATE INDEX idx_items_order_id ON items (order_id);");
}
