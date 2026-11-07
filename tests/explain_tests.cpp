#include <dearsql/explain.hpp>
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
