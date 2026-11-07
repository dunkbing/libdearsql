#include "dearsql/syntax.hpp"

#include "dearsql/completion.hpp"

#include <algorithm>
#include <memory>
#include <pg_query.h>
#include <sqlite3.h>

#ifdef DEARSQL_HAS_DUCKDB
#include <duckdb/common/error_data.hpp>
#include <duckdb/parser/parser.hpp>
#endif

namespace dearsql {

    namespace {

        // a 0-based character position as a byte offset (utf-8)
        size_t byteOffset(std::string_view s, size_t chars) {
            size_t i = 0;
            for (; i < s.size() && chars > 0; --chars) {
                const auto c = static_cast<unsigned char>(s[i]);
                i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            }
            return std::min(i, s.size());
        }

        std::optional<SyntaxError> checkPostgres(std::string_view sql) {
            const std::string text(sql);
            PgQueryParseResult r = pg_query_parse(text.c_str());
            std::optional<SyntaxError> error;
            if (r.error) {
                // cursorpos: 1-based characters, 0 when the error has no position
                const int pos = r.error->cursorpos;
                error = SyntaxError{r.error->message ? r.error->message : "syntax error",
                                    pos > 0 ? byteOffset(sql, static_cast<size_t>(pos - 1)) : 0};
            }
            pg_query_free_parse_result(r);
            return error;
        }

        // "no such table" and friends come from the empty database, not the text
        bool isSqliteSyntaxError(std::string_view msg) {
            return msg.ends_with("syntax error") || msg == "incomplete input" ||
                   msg.starts_with("unrecognized token");
        }

        std::optional<SyntaxError> checkSqlite(std::string_view sql) {
            struct Closer {
                void operator()(sqlite3* db) const {
                    sqlite3_close(db);
                }
            };
            thread_local std::unique_ptr<sqlite3, Closer> db = [] {
                sqlite3* handle = nullptr;
                sqlite3_open(":memory:", &handle);
                return std::unique_ptr<sqlite3, Closer>(handle);
            }();
            if (!db) {
                return std::nullopt;
            }
            // statement by statement: a semantic error stops sqlite's parser mid-statement
            for (const auto& [begin, end] : statementRanges(sql, DatabaseType::SQLITE)) {
                const std::string stmt(sql.substr(begin, end - begin));
                sqlite3_stmt* prepared = nullptr;
                const int rc = sqlite3_prepare_v2(
                    db.get(), stmt.c_str(), static_cast<int>(stmt.size()), &prepared, nullptr);
                sqlite3_finalize(prepared);
                if (rc != SQLITE_OK) {
                    const std::string msg = sqlite3_errmsg(db.get());
                    if (isSqliteSyntaxError(msg)) {
                        const int at = sqlite3_error_offset(db.get());
                        return SyntaxError{msg, begin + (at >= 0 ? static_cast<size_t>(at) : 0)};
                    }
                }
            }
            return std::nullopt;
        }

#ifdef DEARSQL_HAS_DUCKDB
        std::optional<SyntaxError> checkDuckdb(std::string_view sql) {
            try {
                duckdb::Parser parser;
                parser.ParseQuery(std::string(sql));
            } catch (const std::exception& ex) {
                const duckdb::ErrorData err(ex);
                size_t offset = 0;
                if (const auto it = err.ExtraInfo().find("position"); it != err.ExtraInfo().end()) {
                    offset = std::min(static_cast<size_t>(std::stoull(it->second)), sql.size());
                }
                return SyntaxError{err.RawMessage(), offset};
            }
            return std::nullopt;
        }
#endif

    } // namespace

    bool hasSyntaxCheck(DatabaseType type) {
        switch (type) {
        case DatabaseType::POSTGRESQL:
        case DatabaseType::SQLITE:
            return true;
#ifdef DEARSQL_HAS_DUCKDB
        case DatabaseType::DUCKDB:
            return true;
#endif
        default:
            return false;
        }
    }

    bool hasServerSyntaxCheck(DatabaseType type) {
        return type == DatabaseType::COCKROACHDB || type == DatabaseType::MSSQL;
    }

    std::string cockroachSyntaxSql(std::string_view sql) {
        std::string out = "SHOW SYNTAX '";
        for (const char c : sql) {
            out += c == '\'' ? "''" : std::string(1, c);
        }
        return out + "'";
    }

    // rows of (field, message); an error carries "detail": source SQL:\n<statement up to
    // the error line>\n<spaces>^
    std::optional<SyntaxError> cockroachSyntaxError(std::string_view sql,
                                                    const StatementResult& result) {
        std::string message;
        std::string detail;
        for (const auto& row : result.tableData) {
            if (row.size() < 2) {
                continue;
            }
            if (row[0] == "error") {
                message = row[1];
            } else if (row[0] == "detail") {
                detail = row[1];
            }
        }
        if (message.empty()) {
            return std::nullopt;
        }
        SyntaxError error{message, 0};
        constexpr std::string_view head = "source SQL:\n";
        const auto caretLine = detail.rfind('\n');
        if (detail.starts_with(head) && caretLine != std::string::npos && caretLine > head.size()) {
            const std::string source = detail.substr(head.size(), caretLine - head.size());
            const auto caret = detail.find('^', caretLine);
            const auto lastLine = source.rfind('\n');
            // ponytail: first match of the echoed statement; a script repeating it verbatim
            // points at the first copy
            if (const auto at = sql.find(source); at != std::string_view::npos && caret != std::string::npos) {
                error.offset = std::min(sql.size(), at + (lastLine == std::string::npos ? 0 : lastLine + 1) +
                                                        (caret - caretLine - 1));
            }
        }
        return error;
    }

    std::optional<SyntaxError> checkSyntax(std::string_view sql, DatabaseType type) {
        switch (type) {
        case DatabaseType::POSTGRESQL:
            return checkPostgres(sql);
        case DatabaseType::SQLITE:
            return checkSqlite(sql);
#ifdef DEARSQL_HAS_DUCKDB
        case DatabaseType::DUCKDB:
            return checkDuckdb(sql);
#endif
        default:
            return std::nullopt;
        }
    }

} // namespace dearsql
