#pragma once

#include "dearsql/connection_info.hpp"
#include "dearsql/query_result.hpp"

#include <optional>
#include <string>
#include <string_view>

// Syntax checking with each engine's own parser, so a valid statement is never
// flagged: PostgreSQL (libpg_query, the server's grammar), SQLite (sqlite3 prepare
// against an empty in-memory database, syntax errors only) and DuckDB (its parser).
// Two engines check on the server, through IDatabase::checkSyntax: CockroachDB
// (SHOW SYNTAX) and SQL Server (SET PARSEONLY). Every other engine has no
// syntax-only check and is never checked. The functions here do no I/O.
namespace dearsql {

    struct SyntaxError {
        std::string message; // as the engine words it: syntax error at or near "x"
        size_t offset = 0;   // byte offset into the checked text
    };

    [[nodiscard]] bool hasSyntaxCheck(DatabaseType type);

    // nullopt when the text parses, or when the engine has no checker
    [[nodiscard]] std::optional<SyntaxError> checkSyntax(std::string_view sql, DatabaseType type);

    // engines whose check needs a connection: run IDatabase::checkSyntax off the UI thread
    [[nodiscard]] bool hasServerSyntaxCheck(DatabaseType type);

    // where a mysql/mariadb/tidb parse error (1064) points in the statement, from its message
    [[nodiscard]] std::optional<size_t> mysqlParseErrorOffset(std::string_view stmt,
                                                              const std::string& message);

    // CockroachDB's SHOW SYNTAX for the text, and its result read back (pure, for tests)
    [[nodiscard]] std::string cockroachSyntaxSql(std::string_view sql);
    [[nodiscard]] std::optional<SyntaxError> cockroachSyntaxError(std::string_view sql,
                                                                  const StatementResult& result);

} // namespace dearsql
