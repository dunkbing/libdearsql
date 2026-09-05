# libdearsql

Standalone, **synchronous**, SSH-free C++ database client library extracted from
the DearSQL app. Static library only.

The library exposes a small abstraction (`IConnection` → `IDatabase`) that maps
cleanly onto every backend the app talks to. PostgreSQL and MSSQL schemas are
represented as child `IDatabase` handles. SSH tunneling is
intentionally **not** part of the library — the host application is expected to
set up any local port-forwarding and rewrite `ConnectionInfo.host/port` to point
at the tunnel endpoint before calling `open()`.

## Status

Every backend is implemented and covered by the integration suite: SQLite, PostgreSQL (and Redshift), MySQL (and MariaDB), MongoDB, Redis, MSSQL, Oracle, Cassandra. DuckDB has a SQL dialect in `createSQLBuilder` but no connection backend here — DearSQL keeps that one app-side.

The shared API covers connection lifecycle, database/schema discovery, catalog
loading, query execution (with client-side phase timings), table data paging,
table/database DDL, row mutation, column mutation where supported by the backend,
and dialect SQL generation via `createSQLBuilder(DatabaseType)`. Hosts that run
queries in parallel take extra handles from `openDatabase(name)` and cancel a
running one with `IDatabase::cancel()`.

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
│   ├── sql_builder.cpp
│   └── *_connection.cpp
└── tests/
    ├── common_tests.cpp
    ├── sqlite_tests.cpp
    └── *_tests.cpp              # per backend, skip without DEARSQL_TEST_* env
```

## Integration notes

[DearSQL](https://github.com/dunkbing/dearsql) consumes this repo as a git submodule: `add_subdirectory(external/libdearsql)` and link `dearsql::dearsql`. As a subdirectory the library leaves toolchain, triplet and platform flags to the host and skips its tests (`DEARSQL_LIB_BUILD_TESTS` defaults to ON only when built standalone). The vendored driver targets (`freetds_sybdb`/`SYBDB_LIBRARY`, `odpi`, `cassandra_static`) are linked PUBLIC, so a host that still calls the native client libraries gets their headers too.

The library is synchronous. The app should keep async orchestration, progress
state, SSH tunneling, saved-connection metadata, and UI refresh scheduling above
this layer. `ctest` without backend environment variables exercises only common
and SQLite tests; use `scripts/test-remote all` or provide the `DEARSQL_TEST_*`
variables to run the full integration suite.
