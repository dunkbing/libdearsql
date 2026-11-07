#pragma once

#include "dearsql/connection_info.hpp"
#include "dearsql/query_result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Query plans: the EXPLAIN statement for a dialect, and its result parsed into one
// tree shape for every engine. Pure: the host runs the statement itself.
//
// Supported: PostgreSQL (EXPLAIN (FORMAT JSON)), MySQL 8.0.16+ (EXPLAIN FORMAT=TREE,
// EXPLAIN ANALYZE from 8.0.18), SQLite (EXPLAIN QUERY PLAN). Others return nullopt.
// ANALYZE runs the statement: the host decides whether that is safe.
namespace dearsql {

    struct PlanNode {
        std::string operation; // "Seq Scan", "Table scan on t", "SCAN users"
        std::string detail;    // relation, alias and index, when the engine names them

        std::optional<double> estimatedRows; // per loop, as the planner says
        std::optional<double> cost;          // total cost, in the planner's units
        // ANALYZE only
        std::optional<double> actualRows; // per loop
        std::optional<double> loops;
        std::optional<double> totalTimeMs; // whole node, all loops, children included
        std::optional<double> selfTimeMs;  // totalTimeMs minus the children's, never below 0

        // everything else the engine reports (conditions, sort keys, buffers), in its order
        std::vector<std::pair<std::string, std::string>> properties;
        std::vector<PlanNode> children;
    };

    struct Plan {
        PlanNode root;
        bool analyzed = false;
        std::optional<double> planningTimeMs;
        std::optional<double> executionTimeMs;
        std::string raw; // the engine's output as returned, for a text view
    };

    // the EXPLAIN form of one statement; nullopt when this engine has no plan we parse
    [[nodiscard]] std::optional<std::string> explainSql(DatabaseType type,
                                                        std::string_view statement, bool analyze);
    [[nodiscard]] bool supportsExplainAnalyze(DatabaseType type);

    // the result of explainSql's statement; nullopt when it does not parse
    [[nodiscard]] std::optional<Plan> parsePlan(DatabaseType type, const StatementResult& result,
                                                bool analyzed);

} // namespace dearsql
