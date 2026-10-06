# libdearsql

Standalone, **synchronous**, SSH-free C++ database client library. It does all of [DearSQL](https://github.com/dunkbing/dearsql)'s database work — the desktop app, its terminal client and its agent tools are hosts on top of it. Static library only.

The library exposes a small abstraction (`IConnection` → `IDatabase`) that maps cleanly onto every backend. PostgreSQL and MSSQL schemas are child `IDatabase` handles. SSH tunneling is intentionally **not** part of the library — the host sets up any port forwarding and rewrites `ConnectionInfo.host/port` to the tunnel endpoint before calling `open()`.

## Status

Every backend is implemented and covered by the integration suite: SQLite, DuckDB (and CSV files, opened as an in-memory DuckDB table), PostgreSQL (and Redshift), MySQL (and MariaDB), MongoDB, Redis, MSSQL, Oracle, Cassandra. DuckDB is built when CMake finds the `DuckDB` package (`DEARSQL_HAS_DUCKDB`).

The shared API covers connection lifecycle, database/schema discovery, catalog loading (tables with columns, indexes, foreign keys in and out, sizes; views and materialized views with columns and definitions; sequences; routines), query execution (multi-statement, row limits, informational messages, client-side phase timings), table data paging, table/database DDL, row and column mutation, and dialect SQL via `createSQLBuilder(DatabaseType)`.

What a host gets for running work in parallel:

- `IConnection::openDatabase(name)` — a fresh handle with its own connection, for a per-worker pool (`database(name)` returns a shared cached one).
- `IDatabase::cancel()` — best-effort cancel of the query running on that handle, callable from another thread (`KILL QUERY`, `PQcancel`, `sqlite3_interrupt`, `duckdb_interrupt`, `dpiConn_breakExecution`, a flag checked between rows on MSSQL).
- `IDatabase::alive()` — false once the session behind a handle is gone, so a pool can replace it.
- `IDatabase::schema(name)` — a cheap handle for one schema on the same connection (Postgres, MSSQL), no catalog query.

What the library leaves to its host: SSH tunnels, async/threads, pools, progress and UI state, saved-connection storage.

## Contract

- Catalog and data calls (`databases()`, `schemas()`, `tables()`, `views()`, `describeTable()`, `getTableData()`, `getRowCount()`, ...) **throw `dearsql::Error`** on failure. `execute()` never throws; errors are in `QueryResult` (`success()`, `errorMessage()`, per-statement results).
- Mutations return `Status = pair<bool, string>`.
- `rowLimit <= 0` means unlimited.
- SQL `NULL` comes back as `NULL_SENTINEL`, booleans as `BOOL_TRUE_SENTINEL` / `BOOL_FALSE_SENTINEL`, so a UI can tell them from the strings `"NULL"` / `"true"`.
- `IDatabase` implements paging, counting and DDL (create/rename/drop/truncate table, add/rename/alter/drop column, drop view, insert/update/delete row) with the dialect builder over `execute()`, qualified by `schemaName()`; backends override only what their dialect cannot express in SQL (Mongo, Redis).
- `ConnectionInfo::readOnly` opens SQLite and DuckDB read-only; server backends rely on the host refusing writes.

Backend extras, on the concrete classes:

| Backend | Extras |
|---|---|
| PostgreSQL | `PostgresDatabase::handle()` (`PGconn*`), schema handles switch `search_path` only when needed and database-level queries reset it |
| MySQL | `MySQLDatabase::handle()` (`MYSQL*`) for session-scoped work such as dump import/export, `ping()` |
| Redis | `RedisConnection::databaseInfo()` (keys/expires/avg TTL per logical db; db count inferred when `CONFIG GET databases` is refused), `selectedDatabase()`, key helpers |
| Oracle | each schema handle has its own session with `CURRENT_SCHEMA` set; `dearsql::oracle` installer: `install(onProgress, shouldCancel)`, `setInstallRoot(dir)`, `setClientOptions({autoInstall, reexecForLibraryPath})`, `needsClientInstall()`, `resetContext()` |
| MSSQL | `PRINT` / `RAISERROR` (severity ≤ 10) in `QueryResult::messages`; system databases hidden |
| Cassandra | keyspace switch and query under one lock, so concurrent handles never interleave a `USE` |

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

## Layout

```
libdearsql/
├── CMakeLists.txt
├── vcpkg.json                   # manifest with per-backend features
├── cmake/
│   ├── FreeTDS.cmake            # builds db-lib for MSSQL (system libsybdb on linux)
│   ├── OracleOCI.cmake          # builds ODPI-C
│   └── CassandraDriver.cmake    # builds DataStax driver
├── external/                    # vendored submodules (only when needed)
│   ├── freetds/                 # pinned v1.5.14
│   ├── odpi/                    # pinned v5.6.4
│   └── cassandra-cpp-driver/    # pinned 2.17.1
├── include/dearsql/
│   ├── dearsql.hpp              # umbrella header
│   ├── types.hpp                # Column, Index, ForeignKey, Routine, Table
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
│   ├── connection_info.cpp
│   ├── factory.cpp
│   ├── database.cpp             # IDatabase builder-based defaults
│   ├── sql_builder.cpp
│   └── *_connection.cpp
└── tests/
    ├── common_tests.cpp
    ├── sqlite_tests.cpp
    └── *_tests.cpp              # per backend, skip without DEARSQL_TEST_* env
```

## Integration notes

[DearSQL](https://github.com/dunkbing/dearsql) consumes this repo as a git submodule: `add_subdirectory(external/libdearsql)` and link `dearsql::dearsql`. As a subdirectory the library leaves toolchain, triplet and platform flags to the host and skips its tests (`DEARSQL_LIB_BUILD_TESTS` defaults to ON only when built standalone). The vendored driver targets (`freetds_sybdb`, `odpi`, `cassandra_static`) are linked PUBLIC, so a host reaching for a native handle (`handle()`) gets the headers too.

How DearSQL hosts it, as a pattern for other hosts: one `IConnection` per saved connection (opened after the SSH tunnel is up); per database a small pool of `openDatabase(name)` handles, validated with `alive()` and cancelled with `cancel()` from the UI thread; schema views via `schema(name)` on a pooled database handle; catalog calls on worker threads with `dearsql::Error` caught into the sidebar's error text. Its terminal client and MCP server use the library directly and synchronously.

`ctest` without backend environment variables exercises only common and SQLite tests; provide the `DEARSQL_TEST_*` variables (or `scripts/test-remote all`) to run the full integration suite. DearSQL's own `scripts/run-tests` starts every backend in Docker and runs the app-level suite through this library.
