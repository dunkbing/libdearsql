# libdearsql

Standalone, **synchronous**, SSH-free C++ database client library. It does all of [DearSQL](https://github.com/dunkbing/dearsql)'s database work — the desktop app, its terminal client and its agent tools are hosts on top of it. Static library only.

The library exposes a small abstraction (`IConnection` → `IDatabase`) that maps cleanly onto every backend. PostgreSQL and MSSQL schemas are child `IDatabase` handles. SSH tunneling is intentionally **not** part of the library — the host sets up any port forwarding and rewrites `ConnectionInfo.host/port` to the tunnel endpoint before calling `open()`.

## Status

Every backend is implemented and covered by the integration suite: SQLite, DuckDB (and CSV files, opened as a read-only DuckDB view over the file, so queries always see it as saved), PostgreSQL (and Redshift, CockroachDB), MySQL (and MariaDB, TiDB), MongoDB, Redis, MSSQL, Oracle, Cassandra (and ScyllaDB). CockroachDB, TiDB and ScyllaDB run on the Postgres, MySQL and Cassandra backends (`isPostgresFamily()` / `isMySQLFamily()` / `isCassandraFamily()` group the wire-compatible types); the differences handled are CockroachDB's `defaultdb`, its `system` database and `crdb_internal` / `pg_extension` schemas (hidden), `unique_rowid()` serial columns, no `TEMPLATE`/`TABLESPACE` on `CREATE DATABASE` and `SHOW CREATE TABLE` DDL, and TiDB's upper-case system schemas plus `METRICS_SCHEMA` (hidden), and ScyllaDB's keyspaces created with `NetworkTopologyStrategy` (its default tablets refuse `SimpleStrategy`). Cancellation works as on the parent backend. Cassandra. DuckDB is built when CMake finds the `DuckDB` package (`DEARSQL_HAS_DUCKDB`). `DatabaseType::S3` is only a type tag for hosts that browse object storage themselves: it round-trips through the type strings, and `makeConnection` returns null for it.

The shared API covers connection lifecycle, database/schema discovery, catalog loading (tables with columns, indexes, foreign keys in and out, sizes; views and materialized views with columns and definitions; sequences; routines), query execution (multi-statement, row limits, informational messages, client-side phase timings), table data paging, table/database DDL, row and column mutation, and dialect SQL via `createSQLBuilder(DatabaseType)`.

`IDatabase::tableDdl(table)` returns the table's `CREATE TABLE` and its indexes as runnable SQL, for copying a schema elsewhere. The server's own DDL where it has one: MySQL/MariaDB/TiDB and CockroachDB `SHOW CREATE TABLE`, SQLite `sqlite_master`, DuckDB `duckdb_tables()` / `duckdb_indexes()`, Oracle `DBMS_METADATA` (no storage clauses), Redshift `SHOW TABLE`. PostgreSQL has no such function, so it is built from the catalog: `format_type` types, identity, generated and serial columns, every constraint through `pg_get_constraintdef`, indexes not backing a constraint through `pg_get_indexdef`, `PARTITION BY`, `UNLOGGED` and comments (inheritance, ownership and grants are not included). MSSQL and Cassandra use `ISQLBuilder::tableDdl` over `describeTable()`. Throws `Error` for an unknown table and on MongoDB and Redis.

SQL completion (`include/dearsql/completion.hpp`) is pure and I/O-free: `complete(sql, cursor, catalog, type)` returns ranked items and the byte range they replace, `resolveIdentifierAt` maps an identifier to its table/column for hover, `statementRangeAt` finds the statement around an offset, `statementRanges` lists every statement (first to last token, comments skipped), `queryParameters` lists the `:name` placeholders (not in strings or comments, not `::` casts or `:=`) and `bindParameters` replaces them with literals: numbers and NULL as typed, anything else a quoted string, backslashes doubled on MySQL, MariaDB and TiDB. The host does the prompting, and `sqlKeywords`/`sqlFunctions`/`quoteIdentifierIfNeeded` expose the per-dialect tables. It understands strings, comments, quoted identifiers and `$$` bodies, clause context, aliases, CTEs and derived tables. SQL dialects only; Redis and MongoDB return nothing, Cassandra gets CQL keywords.

What a host gets for running work in parallel:

- `IConnection::openDatabase(name)` — a fresh handle with its own connection, for a per-worker pool (`database(name)` returns a shared cached one).
- `IDatabase::cancel()` — best-effort cancel of the query running on that handle, callable from another thread; it never drives the busy connection itself. PostgreSQL: `PQcancel` on a separate cancel object. MySQL: `KILL QUERY <thread id>` from a throwaway connection with 5 s connect/read/write timeouts. SQLite / DuckDB: `sqlite3_interrupt` / `duckdb_interrupt`. Oracle: `dpiConn_breakExecution` (ORA-01013; a running statement stops at once, but a PL/SQL `DBMS_SESSION.SLEEP` only notices an in-band break when it returns, e.g. through Docker Desktop's port forwarding). MSSQL: an atomic flag read by db-lib's interrupt hook, which FreeTDS polls every second while it waits on the server, and between rows; the attention drops the `DBPROCESS`, so `alive()` turns false and the next call reconnects. MongoDB, Redis and Cassandra: no-op (no driver-level cancel of an in-flight request; Cassandra requests time out after 30 s).
- `IDatabase::alive()` — false once the session behind a handle is gone, so a pool can replace it.
- `IDatabase::schema(name)` — a cheap handle for one schema on the same connection (Postgres, MSSQL), no catalog query.

What the library leaves to its host: SSH tunnels, async/threads, pools, progress and UI state, saved-connection storage, and for completion: loading the catalog it runs over and rendering the items.

## Contract

- Catalog and data calls (`databases()`, `schemas()`, `tables()`, `views()`, `describeTable()`, `getTableData()`, `getRowCount()`, ...) **throw `dearsql::Error`** on failure. `execute()` never throws; errors are in `QueryResult` (`success()`, `errorMessage()`, per-statement results).
- Mutations return `Status = pair<bool, string>`.
- `rowLimit <= 0` means unlimited.
- SQL `NULL` comes back as `NULL_SENTINEL`, booleans as `BOOL_TRUE_SENTINEL` / `BOOL_FALSE_SENTINEL`, so a UI can tell them from the strings `"NULL"` / `"true"`.
- `IDatabase` implements paging, counting, reading a table's DDL (`tableDdl()`) and DDL (create/rename/drop/truncate table, add/rename/alter/drop column, drop view, insert/update/delete row) with the dialect builder over `execute()`, qualified by `schemaName()`; backends override only what their dialect cannot express in SQL (Mongo, Redis).
- `ConnectionInfo::readOnly` opens SQLite and DuckDB read-only; server backends rely on the host refusing writes.
- On server backends `IConnection::database()`, `openDatabase()`, `close()` and `dropDatabase()` may be called from different threads (the handle cache is locked); a single `IDatabase` serializes its own calls, except `cancel()`, which is meant to run concurrently.

Backend extras, on the concrete classes:

| Backend | Extras |
|---|---|
| PostgreSQL | `PostgresDatabase::handle()` (`PGconn*`), schema handles switch `search_path` only when needed and database-level queries reset it |
| MySQL | `MySQLDatabase::handle()` (`MYSQL*`) for session-scoped work such as dump import/export, `ping()` |
| DuckDB | `csvMatchingRows(path, where, CsvFormat)` — the 0-based data rows of a CSV file where a SQL condition holds, read on a throwaway in-memory database in file order (`CsvFormat`: delimiter, quote, header, encoding); throws `Error` on a bad condition. For hosts that edit the file themselves and filter it through DuckDB. Writing CSV files is left to the host. |
| Redis | `RedisConnection::databaseInfo()` (keys/expires/avg TTL per logical db; db count inferred when `CONFIG GET databases` is refused), `selectedDatabase()`, key helpers |
| Oracle | each schema handle has its own session with `CURRENT_SCHEMA` set; `dearsql::oracle` installer: `install(onProgress, shouldCancel)`, `setInstallRoot(dir)`, `setClientOptions({autoInstall, reexecForLibraryPath})`, `needsClientInstall()`, `resetContext()` |
| MSSQL | `PRINT` / `RAISERROR` (severity ≤ 10) in `QueryResult::messages`; system databases hidden; `sslmode` Require encrypts, VerifyCA also checks the certificate and hostname against `sslCACertPath` (or the system store), VerifyFull is TDS 8.0 strict with the same checks |
| Cassandra | keyspace switch and query under one lock, so concurrent handles never interleave a `USE`; catalog queries hold a reference to the session, so a `close()` from another thread frees it only once they finish |

## Build

```bash
# clone with submodules so freetds/odpi/cassandra-cpp-driver are present
git submodule update --init --recursive

cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Every backend is built in. The CMake picks up `VCPKG_ROOT`, or falls back to
`~/vcpkg`. All vcpkg dependencies are listed in `vcpkg.json` and pulled
automatically by the manifest mode.

## Usage

```cpp
#include <dearsql/dearsql.hpp>

dearsql::ConnectionInfo info;
info.type = dearsql::DatabaseType::SQLITE;
info.path = "/tmp/example.db";

auto conn = dearsql::makeConnection(info);
auto [ok, err] = conn->open();
if (!ok) { /* handle err */ }

for (auto& db : conn->databases()) {
    // Postgres / MSSQL: drill into schemas (each is another IDatabase).
    // Other backends: schemas() is empty, use db->tables() directly.
    auto tableHolders = db->schemas();
    if (tableHolders.empty())
        tableHolders.push_back(db);

    for (auto& holder : tableHolders) {
        for (const auto& table : holder->tables()) {
            auto rows = holder->getTableData(table, /*limit=*/100, /*offset=*/0);
            auto count = holder->getRowCount(table);
            // ...
        }
    }
}

auto result = conn->database()->execute("SELECT 1, 'hello'");
```

## Completion

```cpp
#include <dearsql/completion.hpp>

dearsql::CompletionCatalog catalog;
catalog.defaultSchema = "public";
for (auto& t : db->tables()) {   // schema = the qualifier a user types: "", "public", "db.dbo"
    t.schema = "public";
    catalog.tables.push_back(t);
}

std::string sql = "SELECT u. FROM users u";
auto result = dearsql::complete(sql, /*cursor=*/9, catalog, dearsql::DatabaseType::POSTGRESQL);
for (const auto& item : result.items) {
    // item.label, item.kind (Column), item.detail (type), item.owner (alias)
}
// apply: sql.replace(result.replaceStart, result.replaceEnd - result.replaceStart, item.insertText)

if (auto id = dearsql::resolveIdentifierAt(sql, 7, catalog, dearsql::DatabaseType::POSTGRESQL)) {
    // id->kind, id->table, id->columnInfo (type, comment) for a hover
}
```

The catalog is a plain value: build it once from whatever the host has loaded (tables and views with columns, sequences, routines) and rebuild when that changes. `complete()` filters by the text typed so far (exact-case prefix, prefix, word boundary, substring, then subsequence for catalog names), drops an item equal to the typed word, dedupes and sorts. `insertText` is quoted for the dialect when a name needs it and qualified with its schema when that is not `defaultSchema`. Tests: `tests/completion_tests.cpp` (no database needed).

## Syntax checks

```cpp
#include <dearsql/syntax.hpp>

if (auto err = dearsql::checkSyntax(sql, type)) {
    // err->message as the engine words it, err->offset a byte offset into sql
}
```

Each engine's own parser, so a valid statement is never flagged: PostgreSQL through [libpg_query](https://github.com/pganalyze/libpg_query) (the server's grammar, vendored in `external/libpg_query` and built by `cmake/PgQuery.cmake`), SQLite by preparing against an empty in-memory database (only syntax errors count, not "no such table"), DuckDB through its parser. Two engines check on the server instead, through `IDatabase::checkSyntax(sql)` (a round trip, so call it off the UI thread; `hasServerSyntaxCheck` names them): CockroachDB runs `SHOW SYNTAX` (its own parser; `cockroachSyntaxSql` / `cockroachSyntaxError` are the pure halves, fixture-tested), SQL Server wraps the text in `SET PARSEONLY ON` / `OFF`, each its own batch (in one batch the text would run), reports the server's line, and drops the session if `OFF` fails, so a pooled connection never stays parse-only. MySQL, MariaDB and TiDB `mysql_stmt_prepare` each statement (`statementRanges`; nothing runs, and the text is never quoted into SQL) and count only error 1064, the parse error: prepare parses the whole statement before it resolves names, so a missing table is a different code. `mysqlParseErrorOffset` reads the position from either wording (`near '…' at line N`, TiDB's `line N column C near "…"`). Checking stops at the first `CREATE PROCEDURE/FUNCTION/TRIGGER/EVENT`, whose body the splitter would cut at its inner `;`. On every other engine `IDatabase::checkSyntax` is the offline check. Oracle is left out on purpose: its parse-only execute also resolves tables and columns and reports both under the same kind of error. Other engines are never checked (`hasSyntaxCheck` is false). libpg_query carries copies of postgres internals that libpq's pgcommon/pgport and libmariadb also define, so its build force-includes `cmake/pg_query_prefix.h`, which renames every global symbol but the `pg_query_*` api; regenerate it after bumping the submodule (instructions in the file). Tests: `tests/syntax_tests.cpp`.

## Query plans

```cpp
#include <dearsql/explain.hpp>

auto sql = dearsql::explainSql(type, "SELECT ...", /*analyze*/ false); // nullopt: no parsed plans
auto result = db->execute(*sql);
if (auto plan = dearsql::parsePlan(type, result.statements.front(), false)) {
    // plan->root: PlanNode tree (operation, detail, estimatedRows, cost,
    // actualRows, loops, totalTimeMs, selfTimeMs, properties, children)
    for (const auto& w : dearsql::reviewPlan(*plan)) {
        // w.node points into *plan, w.message is a sentence for the user or an agent
    }
}
```

Pure, like completion: the host runs the statement. PostgreSQL uses `EXPLAIN (FORMAT JSON)` (`ANALYZE, BUFFERS` with analyze), MySQL 8.0.16+ `EXPLAIN FORMAT=TREE` / `EXPLAIN ANALYZE` (8.0.18+), SQLite `EXPLAIN QUERY PLAN` (tree only, no numbers). Every other type, CockroachDB, Redshift, MariaDB and TiDB included, gets nullopt. Times are wall time: Postgres per-loop time × loops, except below a Gather where workers run concurrently; `selfTimeMs` is a node's time minus its children's. ANALYZE executes the statement, so deciding when that is safe is the host's job.

`reviewPlan` runs fixed rules over the tree and returns hints, each pointing at its node and carrying a finding code (see below): a full scan whose filter keeps under a tenth of what it reads (at least 1,000 rows; with estimates only, a Postgres filtered scan costing 1,000 or more), a full scan on the inner side of a nested loop run 10 or more times (SQLite: any full scan after another scan or search in a join), a sort that spilled to disk or sorts 100,000+ rows, an analyzed row count 10x off its estimate (above 1,000 rows), and SQLite's per-run AUTOMATIC index. It reads no statistics and knows nothing of the data, so the messages are worded as things to check; deciding what to show, and when to suggest an index, is the host's. Tests: `tests/explain_tests.cpp` (fixtures, no database).

## Findings

```cpp
#include <dearsql/findings.hpp>

std::vector<dearsql::Table> tables; // describeTable() each: listings may skip indexes
for (const auto& f : dearsql::reviewIndexes(tables)) {
    // f.code "unindexed-fk" | "redundant-index", f.table, f.message,
    // f.suggestion ("CREATE INDEX idx_orders_user_id ON orders (user_id);")
}
if (const auto* doc = dearsql::findingDoc("unindexed-fk")) {
    // doc->title, meaning, check, fix, ignore: fixed text to show or hand to an agent
}
```

Pure, over tables the host has described. `reviewIndexes` reports a foreign key with no index starting with its columns (any order; the primary key counts, also when the engine does not list it as an index), and an index whose columns lead another index, as an exact duplicate or a prefix of a wider one; the primary key is never reported, and a unique index only gives way to one unique on the same columns. Suggestions are hints with unquoted identifiers, never run by the library; pass the engine (`reviewIndexes(tables, type)`) for its `DROP INDEX` form (MySQL and SQL Server name the table). `findingDocs()` covers every code `reviewIndexes` and `reviewPlan` emit (`filtered-full-scan`, `repeated-full-scan`, `sort-spill`, `big-sort`, `misestimate`, `automatic-index`, `unindexed-fk`, `redundant-index`), engine-neutral, so hosts and agents quote the same words. It knows nothing of query patterns: an index a query needs only by its exact name, or one kept for a hint, still shows up. Tests: `tests/explain_tests.cpp` (`Findings.*`).

## Schema diff

```cpp
#include <dearsql/schema_diff.hpp>

std::vector<dearsql::Table> dev, prod; // describeTable() each, both sides
if (dearsql::sameDialect(devType, prodType)) {
    for (const auto& c : dearsql::diffSchemas(prod, dev, prodType, "public")) {
        // c.kind (CreateTable .. DropTable, the order to run them in), c.table,
        // c.message ("users: add column name text"), c.sql (empty: the engine cannot)
    }
}
```

Pure. The statements make the first side match the second, in the first side's dialect: new tables (`tableDdl`), added, changed and dropped columns (`addColumn`, `alterColumn`, `dropColumn`), indexes matched by columns and uniqueness rather than name (so a renamed index is no change; one that keeps its name but changes columns is dropped and recreated in one step), foreign keys grouped by constraint, and dropped tables last. Views are left out. Where the engine cannot (SQLite changing a column or adding a foreign key) `sql` is empty and the message says so; primary key changes are reported, not scripted. Compare only `sameDialect` pairs: type names from different engines never match. `dropIndexSql(type, index, table)` is the shared `DROP INDEX` form. Tests: `tests/schema_tools_tests.cpp`, and `Postgres.SchemaDiffAppliesCleanly` runs a generated script and diffs again to nothing.

## Profiling

```cpp
#include <dearsql/profile.hpp>

auto r = db->execute(*dearsql::profileSql(type, table), 0); // one bounded scan
auto p = dearsql::parseProfile(type, table, r.statements.back());
// p->rows, p->sampled, per column: nonNull, distinct (-1: not comparable), min, max
auto top = dearsql::topValuesSql(type, table, column, 5); // value, count rows
auto grid = dearsql::profileRowsSql(type, table); // one row per column, to show as is
```

Pure statement builders and a parser; the host runs them. Everything reads the first 100,000 rows the engine returns (`sample`; `LIMIT`, `TOP` or `ROWNUM`), not a random sample, so a big table costs one bounded scan, and only the first 64 columns. `MIN`/`MAX` are left out for types without an order (bool, uuid, json, binary, spatial, arrays) and `COUNT(DISTINCT)` for types without equality (json, lobs, xml, spatial), so one column never fails the statement. SQL engines only (not MongoDB, Redis, Cassandra). Tests: `tests/schema_tools_tests.cpp`, `Postgres.ProfileRunsOnEveryColumnType`.

## Delete impact

```cpp
#include <dearsql/delete_impact.hpp>

// rootWhere picks the rows about to be deleted, unqualified column names
for (const auto& e : dearsql::deleteEffects(type, db->tables(), table, "\"id\" = 1")) {
    // e.table, e.columns, e.action (CASCADE / SET NULL / SET DEFAULT / RESTRICT / NO ACTION),
    // e.path ("customers > orders > items"), e.depth; run e.countSql for the row count
}
```

What deleting rows does to the rows that reference them, one effect per foreign key into the table, following `ON DELETE CASCADE` keys down (`maxDepth`, 3) since those rows go too. Pure: it builds a `COUNT(*)` per effect out of nested `EXISTS` (aliases without `AS`, which Oracle rejects; composite keys join on every column) and the host runs them. Keys are looked up in the `tables` passed in, so a key from another schema is not seen. Two cascade paths into one table count its rows once per path: read a deeper count as "up to". A RESTRICT or NO ACTION effect with rows means the delete fails; a key without a rule counts as NO ACTION. `ForeignKey::onDelete` is filled on PostgreSQL, MySQL, MariaDB, TiDB, SQL Server, Oracle and SQLite (DuckDB has no actions; Redshift does not enforce keys). SQLite connections open with `PRAGMA foreign_keys = ON` (SQLite's own default is off), so the rules apply as declared; a session can turn it off again, so a host may check it. Tests: `tests/delete_impact_tests.cpp` (SQLite, against the real delete) and `DeleteImpactCountsMatch` in each server suite.

## Layout

```
libdearsql/
├── CMakeLists.txt
├── vcpkg.json                   # manifest with per-backend features
├── cmake/
│   ├── FreeTDS.cmake            # builds db-lib for MSSQL from the submodule
│   ├── OracleOCI.cmake          # builds ODPI-C
│   └── CassandraDriver.cmake    # builds DataStax driver
├── external/                    # vendored submodules (only when needed)
│   ├── freetds/                 # pinned v1.5.14
│   ├── odpi/                    # pinned v5.6.4
│   └── cassandra-cpp-driver/    # pinned 2.17.1
├── include/dearsql/
│   ├── dearsql.hpp              # umbrella header
│   ├── types.hpp                # Column, Index, ForeignKey, Routine, Table
│   ├── completion.hpp           # sql completion + identifier resolution (pure)
│   ├── explain.hpp              # explain statements + plan parsing (pure)
│   ├── findings.hpp             # index review + finding docs (pure)
│   ├── schema_diff.hpp          # statements that make one schema match another (pure)
│   ├── profile.hpp              # column profiling statements + parser (pure)
│   ├── syntax.hpp               # syntax checks with each engine's parser
│   ├── query_result.hpp         # StatementResult, QueryResult
│   ├── sql_builder.hpp          # dialect quoting + SQL generation
│   ├── connection_info.hpp      # DatabaseType, SslMode, ConnectionInfo
│   ├── ddl_utils.hpp            # type inference + quoting helpers
│   ├── oracle_installer.hpp     # downloads Oracle Instant Client
│   ├── database.hpp             # IConnection, IDatabase
│   ├── factory.hpp              # makeConnection(info)
│   └── backends/
│       ├── sqlite_connection.hpp
│       ├── duckdb_connection.hpp    # built when DuckDB is found
│       ├── postgres_connection.hpp
│       ├── mysql_connection.hpp
│       ├── mongodb_connection.hpp
│       ├── redis_connection.hpp
│       ├── mssql_connection.hpp
│       ├── oracle_connection.hpp
│       └── cassandra_connection.hpp
├── src/
│   ├── types.cpp
│   ├── completion.cpp
│   ├── explain.cpp
│   ├── findings.cpp
│   ├── connection_info.cpp
│   ├── factory.cpp
│   ├── database.cpp             # IDatabase builder-based defaults
│   ├── sql_builder.cpp
│   └── *_connection.cpp
└── tests/
    ├── common_tests.cpp
    ├── completion_tests.cpp     # pure, no database
    ├── explain_tests.cpp        # plan parsers on fixtures, no database
    ├── syntax_tests.cpp         # parser checks, no database
    ├── sqlite_tests.cpp
    └── *_tests.cpp              # per backend, skip without DEARSQL_TEST_* env
```

## Integration notes

[DearSQL](https://github.com/dunkbing/dearsql) consumes this repo as a git submodule: `add_subdirectory(external/libdearsql)` and link `dearsql::dearsql`. As a subdirectory the library leaves toolchain, triplet and platform flags to the host and skips its tests (`DEARSQL_LIB_BUILD_TESTS` defaults to ON only when built standalone). The vendored driver targets (`freetds_sybdb`, `odpi`, `cassandra_static`) are linked PUBLIC, so a host reaching for a native handle (`handle()`) gets the headers too.

How DearSQL hosts it, as a pattern for other hosts: one `IConnection` per saved connection (opened after the SSH tunnel is up); per database a small pool of `openDatabase(name)` handles, validated with `alive()` and cancelled with `cancel()` from the UI thread; schema views via `schema(name)` on a pooled database handle; catalog calls on worker threads with `dearsql::Error` caught into the sidebar's error text. Its terminal client and MCP server use the library directly and synchronously.

`ctest` without backend environment variables exercises only common and SQLite tests; provide the `DEARSQL_TEST_*` variables (or `scripts/test-remote all`) to run the full integration suite. DearSQL's own `scripts/run-tests` starts every backend in Docker and runs the app-level suite through this library.
