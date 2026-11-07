#pragma once

#include "dearsql/connection_info.hpp"

#include <optional>
#include <string>
#include <string_view>

// Syntax checking with each engine's own parser, so a valid statement is never
// flagged: PostgreSQL (libpg_query, the server's grammar), SQLite (sqlite3 prepare
// against an empty in-memory database, syntax errors only) and DuckDB (its parser).
// Every other engine has no embeddable parser and is never checked. No I/O.
namespace dearsql {

    struct SyntaxError {
        std::string message; // as the engine words it: syntax error at or near "x"
        size_t offset = 0;   // byte offset into the checked text
    };

    [[nodiscard]] bool hasSyntaxCheck(DatabaseType type);

    // nullopt when the text parses, or when the engine has no checker
    [[nodiscard]] std::optional<SyntaxError> checkSyntax(std::string_view sql, DatabaseType type);

} // namespace dearsql
