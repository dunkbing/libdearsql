#pragma once

// what deleting rows does to the rows that reference them: one effect per foreign key into
// the table (and, through ON DELETE CASCADE, into the rows that cascade away), each with a
// COUNT(*) query for the host to run. pure: builds SQL, runs nothing.

#include "connection_info.hpp"
#include "types.hpp"
#include <string>
#include <vector>

namespace dearsql {

    struct DeleteEffect {
        std::string table;   // the referencing table
        std::string columns; // its referencing columns, "a, b"
        // CASCADE (deleted too), SET NULL / SET DEFAULT (updated), RESTRICT / NO ACTION
        // (the delete fails while any such row exists); a key without a rule is NO ACTION
        std::string action;
        std::string path;     // tables from the deleted rows down to this one, "a > b > c"
        int depth = 1;        // 1: references the deleted rows directly
        std::string countSql; // SELECT COUNT(*) of the affected rows
    };

    // rootWhere picks the rows being deleted from `root`, with unqualified column names; it is
    // evaluated in the root table's own scope, so a self-referencing key reads it right.
    // `tables` is the catalog the keys are looked up in (foreignKeys; one schema). cascades
    // are followed `maxDepth` levels down; two cascade paths into one table count its rows
    // once per path, so treat a count as "up to".
    [[nodiscard]] std::vector<DeleteEffect>
    deleteEffects(DatabaseType type, const std::vector<Table>& tables, const Table& root,
                  const std::string& rootWhere, int maxDepth = 3);

} // namespace dearsql
