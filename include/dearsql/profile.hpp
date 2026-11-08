#pragma once

#include "dearsql/connection_info.hpp"
#include "dearsql/query_result.hpp"
#include "dearsql/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Column profiling: per column, how many values are null and distinct, the smallest and
// largest, and the most common ones. Pure: the library builds the statements and reads
// their results; the host runs them. Everything is computed over the first `sample` rows
// the engine returns (not a random sample), so a big table costs one bounded scan.
namespace dearsql {

    struct ColumnProfile {
        std::string column;
        std::string type;
        int64_t nonNull = 0;
        int64_t distinct = -1; // -1: the type cannot be compared (json, blobs)
        std::string min, max;  // empty when the type has no order (bool, uuid, json)
        bool hasMinMax = false;
    };

    struct TableProfile {
        int64_t rows = 0;     // rows profiled, at most the sample
        bool sampled = false; // rows reached the sample size: the table may be bigger
        std::vector<ColumnProfile> columns;
    };

    // one statement for the whole table (the first 64 columns), or nullopt for engines
    // without SQL (MongoDB, Redis, Cassandra)
    [[nodiscard]] std::optional<std::string> profileSql(DatabaseType type, const Table& table,
                                                        int sample = 100000);
    // the same profile as a readable result: one row per column (column, type, nulls, distinct,
    // min, max, rows), for a host to show in a grid. min and max come back as text
    [[nodiscard]] std::optional<std::string> profileRowsSql(DatabaseType type, const Table& table,
                                                            int sample = 100000);
    // the result of profileSql's statement
    [[nodiscard]] std::optional<TableProfile> parseProfile(DatabaseType type, const Table& table,
                                                           const StatementResult& result,
                                                           int sample = 100000);

    // the `limit` most common values of one column with their counts (value, count rows),
    // over the same sample; nullopt when the engine or the type cannot group
    [[nodiscard]] std::optional<std::string> topValuesSql(DatabaseType type, const Table& table,
                                                          const Column& column, int limit = 5,
                                                          int sample = 100000);

} // namespace dearsql
