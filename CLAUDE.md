# libdearsql — Agent Guide

> Synchronous, SSH-free C++ database client library. DearSQL does all its database work through it.
> Static library, vcpkg-managed deps, GoogleTest-based suite.

## Project Overview

`libdearsql` is a C++20 static library that exposes a uniform synchronous API
over SQLite, DuckDB (and CSV), PostgreSQL/Redshift/CockroachDB, MySQL/MariaDB/TiDB, MongoDB, Redis,
MSSQL, Oracle, and Cassandra. Every SQL string and driver call of the DearSQL
app lives here; the app (and its terminal client / MCP server) is the host and
adds only tunnels, threads, pools and UI state. New database behaviour is
implemented here first. SSH tunneling
lives in the app — the lib only sees the local endpoint after the app rewrites
`ConnectionInfo.host/port`.

## Public API (three nouns, no async)

```
IConnection        — server-level handle. open()/close(), databases(), database(name),
                     openDatabase(name) for a fresh uncached handle (host-side pools)
  └── IDatabase    — a database/keyspace, also serves as the Postgres/MSSQL "schema"
                     when reached via IDatabase::schemas(). Owns tables/views/etc.
                     cancel() is a best-effort server-side kill of its running query.
                     alive() for pool validation; schema(name) = cheap schema view on
                     the same connection; schemaName() qualifies the builder defaults.
```

Backends may expose the concrete handle class for callers that need the native
connection: `MySQLDatabase::handle()` (`MYSQL*`, DearSQL's dump code), `PostgresDatabase::handle()` (`PGconn*`), `SQLiteConnection::handle()`.

No `ISchema` — Postgres/MSSQL expose schemas by returning more `IDatabase`s from
`schemas()`. Every other backend's `schemas()` returns empty; you call
`db->tables()` directly. `execute()` lives on `IDatabase`, not `IConnection`.

SQL completion is the one piece that touches no database: `include/dearsql/completion.hpp` /
`src/completion.cpp` hold a pure engine (tokenizer, statement splitter, clause
context, alias/CTE/derived-table scopes, dialect keyword and function tables,
quoting). `complete()`, `resolveIdentifierAt()` and `statementRangeAt()` take
`(sql, byte offset, CompletionCatalog, DatabaseType)` and nothing else, so the
GUI editor, the TUI and a language server share one implementation. It leaves to
the host: loading the catalog (tables/views with columns, sequences, routines) from
the `IDatabase` calls, deciding when to rebuild it, and all rendering. Keep it
I/O-free and allocation-light (it runs per keystroke); new SQL-language tooling
(hover, signature help) belongs next to it.

Syntax checks too: `include/dearsql/syntax.hpp` / `src/syntax.cpp` run the engine's own
parser (libpg_query for Postgres, sqlite3 prepare on an empty `:memory:` db, DuckDB's
parser) and never guess for engines without one. CockroachDB (`SHOW SYNTAX`) and SQL
Server (`SET PARSEONLY`, its own batch each side; the session is dropped if `OFF` fails)
check on the server through `IDatabase::checkSyntax`, as do MySQL, MariaDB and TiDB
(`mysql_stmt_prepare` per statement, only error 1064 counts; stops at a routine definition);
the default is the offline check. libpg_query is vendored and built by
`cmake/PgQuery.cmake` with every non-api symbol renamed (`cmake/pg_query_prefix.h`,
generated) so it links next to libpq and libmariadb.

Query plans are the same kind of thing: `include/dearsql/explain.hpp` / `src/explain.cpp`
build the EXPLAIN statement per dialect (`explainSql`) and parse what the server
returns into one `PlanNode` tree (`parsePlan`): Postgres JSON, MySQL `FORMAT=TREE`
text, SQLite `EXPLAIN QUERY PLAN` rows. `reviewPlan` runs fixed rules over that tree
(filtered and repeated full scans, big or spilled sorts, misestimates, SQLite automatic
indexes) and returns worded hints pointing at nodes; keep it rule-based and data-free. No I/O; the host runs it and decides when
ANALYZE (which executes the statement) is safe.

Delete impact too: `include/dearsql/delete_impact.hpp` (`deleteEffects`) turns the
catalog's foreign keys (`onDelete` read on every server engine and SQLite) into one
`COUNT(*)` per effect, nested `EXISTS` down cascade chains, for the host to run. SQLite
keys get one name per constraint id, so a composite key stays one `ForeignKey` group.

Common types live in `dearsql::` namespace:
- `Column`, `Index`, `ForeignKey`, `Routine`, `Table` (`include/dearsql/types.hpp`)
- `StatementResult`, `QueryResult` (`include/dearsql/query_result.hpp`)
- `ConnectionInfo`, `DatabaseType`, `SslMode` (`include/dearsql/connection_info.hpp`)
- `Status = std::pair<bool, std::string>` for success/error returns

## Repository Layout

```
libdearsql/
├── CMakeLists.txt                 — single static target `dearsql` (alias `dearsql::dearsql`)
├── vcpkg.json                     — manifest, all backend deps unconditionally
├── README.md                      — user-facing
├── CLAUDE.md                      — this file
├── cmake/
│   ├── FreeTDS.cmake              — builds db-lib for MSSQL from the submodule, every platform
│   ├── freetds-login-cafile.patch — applied to the pinned FreeTDS at configure: db-lib
│   │                                otherwise drops the login's CA file (sslmode verify-ca)
│   ├── OracleOCI.cmake            — builds ODPI-C
│   └── CassandraDriver.cmake      — builds DataStax C++ driver
├── external/                      — vendored submodules
│   ├── freetds/                   — pinned v1.5.14
│   ├── odpi/                      — pinned v5.6.4
│   ├── libpg_query/               — pinned 17-6.2.5 (postgres parser)
│   └── cassandra-cpp-driver/      — pinned 2.17.1
├── include/dearsql/
│   ├── dearsql.hpp                — umbrella
│   ├── types.hpp / query_result.hpp / connection_info.hpp / database.hpp / factory.hpp
│   ├── completion.hpp             — pure sql completion + identifier resolution
│   ├── delete_impact.hpp          — what a delete does through foreign keys (pure)
│   ├── explain.hpp                — explain statements + plan parsing (pure)
│   ├── syntax.hpp                 — syntax checks with each engine's parser
│   └── backends/                  — one header per backend
├── src/
│   ├── types.cpp / connection_info.cpp / factory.cpp
│   ├── completion.cpp             — tokenizer, scopes, ranking, dialect word lists
│   └── *_connection.cpp           — one per backend
├── docker/
│   └── docker-compose.yml         — 8-service stack for integration tests
├── scripts/
│   └── test-remote                — SSH + tunnel + ctest driver
└── tests/
    ├── test_helpers.hpp           — env loader, tryOpen(), SKIP macros
    ├── common_tests.cpp           — type/helper unit tests (always run)
    ├── completion_tests.cpp       — completion engine (pure, always run; also built into
    │                                DearSQL's sql_format_tests)
    ├── sqlite_tests.cpp           — 13 in-memory SQLite tests (always run)
    └── *_tests.cpp                — per-backend; skip when env unset or backend stub
```

## Backend Status

All backends are implemented, DuckDB included (`src/duckdb_connection.cpp`, built
when CMake finds DuckDB; `.csv` paths open as a read-only view over the file, and
`csvMatchingRows()` filters a CSV with a SQL condition for hosts that edit it). Every server
backend has `openDatabase()`, `cancel()` and `alive()` (cancel is a no-op on MongoDB,
Redis and Cassandra; README "Status" lists the mechanism per backend — never drive
the busy handle from the cancelling thread); Postgres/MSSQL have a cheap
`schema(name)`. See README "Contract" for the error and sentinel rules.
`QueryResult` also carries `messages` (mssql PRINT output) and `phaseTimings`
(postgres/mysql client-side timings) because the app renders them.
`tableDdl()` is the one catalog call with per-dialect SQL in `src/database.cpp`
(a switch on `type()`, so schema views get it through `schemaName()`); Postgres
builds it from the catalog, the rest ask the server or fall back to
`ISQLBuilder::tableDdl` over `describeTable()`.

Per-backend test counts (run `./build/tests/dearsql_lib_tests --gtest_list_tests`):

| Backend       | Tests | Notes                                                  |
| ------------- | ----- | ------------------------------------------------------ |
| SQLite        | 16    | full, + table DDL round trip and builder fallback      |
| DuckDB        | 6     | + foreign keys, csv view and row filter, parallel handles, DDL |
| PostgreSQL    | 17    | + schemas, sequences, routines, matviews, catalog DDL  |
| MySQL         | 14    | + AUTO_INCREMENT detect, routines, SHOW CREATE TABLE   |
| MSSQL         | 11    | + dbo schema, OFFSET/FETCH paging, builder DDL         |
| Oracle        | 9     | + sequences, FREEPDB1 PDB, DBMS_METADATA DDL           |
| MongoDB       | 7     | + collections-as-tables, JSON command execute          |
| Redis         | 8     | + key types, TTL, SCAN, SELECT db                      |
| Cassandra     | 8     | + keyspace as database, CQL                            |
| Redshift      | 3     | reuses Postgres backend                                |
| CockroachDB   | —     | Postgres backend; app runs the Postgres suite on it    |
| TiDB          | —     | MySQL backend; app runs the MySQL suite on it          |
| Common        | 7     | type/helper unit tests                                 |
| Completion    | 17    | pure: contexts, scopes, quoting, dialects, resolve     |
| Explain       | 10    | pure: postgres json, mysql tree, sqlite rows, plan review |
| Syntax        | 5     | pg, sqlite, duckdb parsers; none for the rest          |

## Build

Standalone, or as a subdirectory of DearSQL (`external/libdearsql`, linked as
`dearsql::dearsql`). As a subdirectory the toolchain/triplet/platform flags and
the tests are left to the host; driver targets are linked PUBLIC so the app can
still reach `sybdb.h`, `dpi.h` and `cassandra.h` while it migrates.

```bash
git submodule update --init --recursive  # freetds, odpi, cassandra-cpp-driver
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`CMakeLists.txt` honors `VCPKG_ROOT` then falls back to `~/vcpkg`. On macOS it
sets `CMAKE_OSX_DEPLOYMENT_TARGET=14.0` to match libc++ experimental.

## Integration Tests

Tests skip cleanly when env vars are unset. To run them against real DBs,
either point to a local stack or use the bundled remote runner.

### Local (your own docker)

```bash
docker compose -f docker/docker-compose.yml up -d
export DEARSQL_TEST_PG_HOST=127.0.0.1 DEARSQL_TEST_PG_PORT=55432 ...
ctest --test-dir build
```

### Remote (SSH + tunnel)

```bash
./scripts/test-remote up      # deploy compose to remote, start containers, open tunnel
./scripts/test-remote test    # run ctest
./scripts/test-remote down    # close tunnel + compose down
./scripts/test-remote all     # full cycle (default)
```

Override the host with `DEARSQL_REMOTE_HOST=user@box ./scripts/test-remote ...`.

### CI

`.github/workflows/test-linux.yml` runs the full suite on ubuntu-24.04 with
vcpkg caching and the docker stack. Trigger via the **Actions** tab → "Run
workflow".

## Host Integration (DearSQL)

DearSQL (`external/libdearsql` in the app) wraps one `IConnection` per saved
connection in `ServerDatabase<NodeT>` / `FileDatabase`, and each database in a
`LibDatabaseNode` that pools `openDatabase(name)` handles (validated with
`alive()`, cancelled with `cancel()`), runs catalog calls on worker threads and
shows `dearsql::Error` text in the sidebar. Postgres/MSSQL schema nodes run on
their database node's pooled handle via `schema(name)`. Its `--tui` /
`--mcp` terminal modes call the library synchronously. A change
here that alters an interface must keep those callers compiling — build the app
and run its `scripts/run-tests` (Docker) when in doubt.

## Conventions

- C++20: `std::format`, ranges, structured bindings, `[[nodiscard]]`.
- Headers `.hpp` under `include/dearsql/`, implementations `.cpp` under `src/`.
- Headers stay platform-independent. Backend cpp files include their native
  driver header.
- All public types in namespace `dearsql`.
- NULL → `dearsql::NULL_SENTINEL` (`"dearsql__null"`) in result strings so
  callers can distinguish SQL NULL from the literal `"NULL"`.
- bool → `BOOL_TRUE_SENTINEL` / `BOOL_FALSE_SENTINEL` so UI layers can render
  checkboxes.
- Errors: catalog and data calls throw `dearsql::Error` (never swallow with
  `catch (...)` and return empty — hosts show the message); mutations return
  `Status = pair<bool, string>`; `execute()` reports in `QueryResult`
  (`success()` aggregates over multi-statement results). `rowLimit <= 0` = unlimited.
- Paging/counting/DDL come from the `IDatabase` builder defaults (`src/database.cpp`)
  qualified by `schemaName()`; don't hand-roll SQL a dialect builder can produce.
- Quote identifiers with the builder and escape literals (`ddl_utils::escapeSingleQuotes`).
- No spdlog. No imgui. No SSH. No UI state. No comments narrating obvious
  code.

## Maintenance Checklist

Before pushing a change:

- [ ] `cmake --build build` succeeds on macOS arm64 (local dev).
- [ ] `ctest --test-dir build` passes (SQLite + common assertions).
- [ ] Backend ports include a Docker-backed integration test pass.
- [ ] New sources added to `CMakeLists.txt` (the static target's source list).
- [ ] If a backend's public surface changes, mirror the change in
      `include/dearsql/backends/<name>_connection.hpp` and the corresponding
      test file.
- [ ] If a vcpkg dep is added or its feature set changes, bump the cache key
      suffix in `.github/workflows/test-linux.yml`.

## Notes for AI Agents

1. **Scope discipline**: this lib is database operations only. (SQL completion is
   the exception: pure SQL-language analysis, no I/O, so every host shares it.) If you find
   yourself reaching for UI, logging frameworks, threading, async, or SSH —
   stop. Those concerns belong upstream in the app.
2. **Stable interfaces**: changing `IConnection` / `IDatabase` ripples through
   every backend header + cpp + test. When changing them, grep
   `include/dearsql/backends/` to keep stubs honest.
3. **Test gating**: when you add a new test that depends on a real DB, gate
   it with both `DEARSQL_SKIP_IF_NO_CONFIG` (env vars present) and
   `DEARSQL_SKIP_IF_UNIMPLEMENTED` (backend body present). Read
   `tests/test_helpers.hpp` for the helpers.
4. **Don't reach into the app**: even though the lib lives under `dearsql/`,
   the lib must compile with no knowledge of the app's headers. Treat the
   parent repo as read-only context.
