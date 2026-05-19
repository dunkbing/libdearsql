# libdearsql

Standalone, **synchronous**, SSH-free C++ database client library extracted from
the DearSQL app. Static library only.

The library exposes a small abstraction (`IConnection` → `IDatabase` → `ISchema`)
that maps cleanly onto every backend the app talks to. SSH tunneling is
intentionally **not** part of the library — the host application is expected to
set up any local port-forwarding and rewrite `ConnectionInfo.host/port` to point
at the tunnel endpoint before calling `open()`.

## Status

| Backend       | API surface | Implementation       |
| ------------- | ----------- | -------------------- |
| SQLite        | ✅           | ✅ Full + tested     |
| PostgreSQL    | ✅           | ⏳ Skeleton (port from app) |
| Redshift      | ✅           | ⏳ Reuses Postgres   |
| MySQL         | ✅           | ⏳ Skeleton          |
| MariaDB       | ✅           | ⏳ Reuses MySQL      |
| MongoDB       | ✅           | ⏳ Skeleton          |
| Redis         | ✅           | ⏳ Skeleton          |
| MSSQL         | ✅           | ⏳ Skeleton          |
| Oracle        | ✅           | ⏳ Skeleton          |
| Cassandra     | ✅           | ⏳ Skeleton          |

Each skeleton compiles and links; calling `open()` returns `{false, "...not
implemented yet"}` until the body is ported from the app's corresponding
`src/database/*.cpp`.

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
│   ├── connection_info.hpp      # DatabaseType, SslMode, ConnectionInfo
│   ├── database.hpp             # IConnection, IDatabase, ISchema
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
│   ├── sqlite_connection.cpp    # fully implemented
│   └── *_connection.cpp         # skeletons returning "not implemented"
└── tests/
    ├── common_tests.cpp
    └── sqlite_tests.cpp
```

## Porting a backend

Each `src/<name>_connection.cpp` carries a `// TODO:` comment pointing at the
files in the app source tree to copy from. The mechanical work is:

1. Drop every `AsyncOperation<...>` member, every `Async` method, and the
   `start*LoadAsync` / `check*StatusAsync` pairs — replace with direct
   synchronous calls.
2. Drop `ConnectionPool<T>`; hold one connection handle behind a mutex if
   thread-safety is needed.
3. Drop UI state (`attemptedConnection`, `lastConnectionError`,
   `savedConnectionId`, `*expanded` booleans).
4. Drop SSH (`sshTunnel_`, `prepareConnectionForConnect`, `stopSshTunnel`).
5. Wrap the result behind `IConnection` / `IDatabase` / `ISchema`, mapping the
   app's two-tier (or three-tier for Postgres/MSSQL) hierarchy onto the
   library's interfaces.

See `src/sqlite_connection.cpp` for the worked reference.
