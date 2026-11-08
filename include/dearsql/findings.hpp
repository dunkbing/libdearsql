#pragma once

#include "dearsql/connection_info.hpp"
#include "dearsql/types.hpp"

#include <string>
#include <string_view>
#include <vector>

// Schema and plan findings: what reviewPlan (explain.hpp) and reviewIndexes report,
// each under a stable code with a fixed explanation, so a host or an agent can quote
// the same words instead of inventing them. Pure: the host loads the tables
// (describeTable, for indexes) and decides what to show and what to run.
namespace dearsql {

    // what a finding code means; every field is plain text, engine-neutral
    struct FindingDoc {
        std::string_view code;    // "unindexed-fk"
        std::string_view title;   // one line
        std::string_view meaning; // what was seen and why it matters
        std::string_view check;   // how to confirm it before acting
        std::string_view fix;     // the usual remedy
        std::string_view ignore;  // when it is fine to leave it
    };
    [[nodiscard]] const std::vector<FindingDoc>& findingDocs();
    // the doc for a code, or nullptr
    [[nodiscard]] const FindingDoc* findingDoc(std::string_view code);

    struct IndexFinding {
        std::string code; // "unindexed-fk", "redundant-index"
        std::string table;
        std::string message;
        std::string suggestion; // a statement to consider, identifiers unquoted; may be empty
    };
    // tables as describeTable returns them (columns, indexes, foreign keys). a foreign key
    // with no index starting with its columns (the primary key counts), and an index whose
    // columns lead another index (an exact duplicate, or a prefix of a wider one), unless
    // it is the primary key or a unique index the other does not replace. type: the engine,
    // for the DROP INDEX form (MySQL and SQL Server name the table)
    [[nodiscard]] std::vector<IndexFinding> reviewIndexes(const std::vector<Table>& tables,
                                                            DatabaseType type = DatabaseType::SQLITE);

} // namespace dearsql
