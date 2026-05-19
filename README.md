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

| Backend       | API surface | Implementation       |
| ------------- | ----------- | -------------------- |
| SQLite        | ✅           | ✅ Full + tested     |
| PostgreSQL    | ✅           | ✅ Full + tested     |
| Redshift      | ✅           | ✅ Reuses Postgres   |
| MySQL         | ✅           | ✅ Full + tested     |
| MariaDB       | ✅           | ✅ Reuses MySQL      |
| MongoDB       | ✅           | ✅ Full + tested     |
| Redis         | ✅           | ✅ Full + tested     |
| MSSQL         | ✅           | ✅ Full + tested     |
| Oracle        | ✅           | ✅ Full + tested     |
| Cassandra     | ✅           | ✅ Full + tested     |

The shared API covers connection lifecycle, database/schema discovery, catalog
loading, query execution, table data paging, table/database DDL, row mutation,
column mutation where supported by the backend, and dialect SQL generation via
`createSQLBuilder(DatabaseType)`.

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
│   ├── FreeTDS.cmake            # builds db-lib for MSSQL
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
    └── sqlite_tests.cpp
```

## Integration notes

The library is synchronous. The app should keep async orchestration, progress
state, SSH tunneling, saved-connection metadata, and UI refresh scheduling above
this layer. `ctest` without backend environment variables exercises only common
and SQLite tests; use `scripts/test-remote all` or provide the `DEARSQL_TEST_*`
variables to run the full integration suite.
