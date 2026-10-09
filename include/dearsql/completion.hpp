#pragma once

// sql completion and identifier resolution.
//
// pure functions of (buffer, byte offset, catalog, dialect): no i/o, no
// threads, no state. the host loads the catalog (tables, views, columns,
// sequences, routines) however it likes and renders the results; the same
// calls back a gui editor, a terminal client and a language server.
//
// what it understands:
// - a real tokenizer: '…' strings, "…" / `…` / […] identifiers per dialect,
//   -- and /* */ comments (# on mysql), postgres $$ / $tag$ bodies, numbers,
//   parameters. nothing is completed inside a string, comment or number.
// - only the statement holding the cursor (split on ';' outside quotes).
// - clause context: FROM / JOIN / UPDATE / INTO / TABLE offer relations and
//   schemas; SELECT / WHERE / ON / GROUP BY / ORDER BY / HAVING / SET offer
//   the columns of the tables in scope first, then functions and keywords;
//   `alias.`, `table.` and `schema.` offer members only.
// - aliases (`users u`, `users AS u`), ctes (`WITH x AS (…)`, columns from
//   the cte's select list or column list), derived tables (`(SELECT …) s`),
//   correlated scopes (an inner subquery sees outer aliases).

#include "connection_info.hpp"
#include "types.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dearsql {

enum class CompletionKind : uint8_t {
    Keyword,
    Function,
    Table,
    View,
    Column,
    Schema,
    Sequence,
    Alias
};

struct CompletionItem {
    std::string label; // display text, unquoted
    CompletionKind kind = CompletionKind::Keyword;
    std::string detail;     // column type, routine signature, "cte", aliased table...
    std::string owner;      // table/alias owning a column, schema of a relation; may be empty
    std::string insertText; // replaces [replaceStart, replaceEnd); dialect-quoted and
                            // schema-qualified when needed
    int score = 0;          // higher is better; results come sorted by it
};

// a schema-level object known only by name (sequences)
struct CatalogObject {
    std::string schema;
    std::string name;
};

// what the completer may offer. `schema` on every entry is the qualifier
// path a user types before the name: "" when there is none, "public", or a
// dotted path for two levels ("sales_db.dbo"). typed qualifiers match a
// suffix of the path, so `dbo.` finds "sales_db.dbo" objects.
struct CompletionCatalog {
    std::vector<Table> tables; // name, schema, columns (name, type) are read
    std::vector<Table> views;
    std::vector<CatalogObject> sequences;
    std::vector<Routine> routines;    // user functions/procedures
    std::vector<std::string> schemas; // extra qualifier paths; those of the objects
                                      // above are offered automatically
    // objects in this schema insert unqualified; others insert "schema.name"
    // (leading segments shared with it are dropped: "db.sales" -> "sales")
    std::string defaultSchema;
};

struct CompletionResult {
    std::vector<CompletionItem> items;
    // byte range the chosen item's insertText replaces: the whole identifier
    // under the cursor (quotes included), or an empty range at the cursor
    size_t replaceStart = 0;
    size_t replaceEnd = 0;
};

// completions at `cursor` (a byte offset into `sql`). items are filtered by
// the text between the identifier start and the cursor (exact-case prefix >
// prefix > word-boundary > substring > subsequence for catalog names), deduped
// and sorted; an item equal to the typed word is left out. empty for redis
// and mongodb, and inside strings, comments and numbers.
[[nodiscard]] CompletionResult complete(std::string_view sql, size_t cursor,
                                        const CompletionCatalog& catalog, DatabaseType type);

// the catalog object an identifier stands for, for hover/go-to-definition
struct ResolvedIdentifier {
    // Table, View, Column, Schema, Sequence, or Alias for a cte / derived table
    CompletionKind kind = CompletionKind::Table;
    std::string schema;           // qualifier path of the relation, when known
    std::string relation;         // table/view/cte name (for a column: its owner)
    std::string column;           // set for Column
    std::string alias;            // the alias the identifier went through, if any
    const Table* table = nullptr; // into the catalog; null for ctes/derived tables
    const Column* columnInfo = nullptr;
    size_t start = 0; // byte range of the identifier token
    size_t end = 0;
};

// resolves the identifier at `offset` (alias.column, schema.table, a bare
// column of a table in scope, a cte...). nullopt for keywords, literals and
// names the catalog does not know.
[[nodiscard]] std::optional<ResolvedIdentifier>
resolveIdentifierAt(std::string_view sql, size_t offset, const CompletionCatalog& catalog,
                    DatabaseType type);

// byte range [first, second) of the statement holding `offset`, split on ';'
// outside strings, comments and quoted identifiers; leading whitespace and
// the ';' itself are excluded
[[nodiscard]] std::pair<size_t, size_t> statementRangeAt(std::string_view sql, size_t offset,
                                                         DatabaseType type);

// uppercase, sorted, per dialect; empty for redis and mongodb, cql for cassandra
[[nodiscard]] const std::vector<std::string>& sqlKeywords(DatabaseType type);
[[nodiscard]] const std::vector<std::string>& sqlFunctions(DatabaseType type);

// true when `name` must be quoted to be read back as written: odd characters,
// a reserved word, or a case the dialect would fold (postgres/cassandra
// uppercase, oracle lowercase)
[[nodiscard]] bool identifierNeedsQuoting(std::string_view name, DatabaseType type);
// `name`, quoted with the dialect's quotes only when needed
[[nodiscard]] std::string quoteIdentifierIfNeeded(std::string_view name, DatabaseType type);

} // namespace dearsql
