# libdearsql — Agent Guide

> Synchronous, SSH-free C++ database client library extracted from DearSQL.
> Static library, vcpkg-managed deps, GoogleTest-based suite.

## Project Overview

`libdearsql` is a C++20 static library that exposes a uniform synchronous API
over SQLite, PostgreSQL/Redshift, MySQL/MariaDB, MongoDB, Redis, MSSQL,
Oracle, and Cassandra. It was extracted from the DearSQL desktop app's
database layer; the app stays the host, the lib stays headless. SSH tunneling
lives in the app — the lib only sees the local endpoint after the app rewrites
`ConnectionInfo.host/port`.

## Public API (three nouns, no async)

```
IConnection        — server-level handle. open()/close(), databases(), database(name)
  └── IDatabase    — a database/keyspace, also serves as the Postgres/MSSQL "schema"
                     when reached via IDatabase::schemas(). Owns tables/views/etc.
```

No `ISchema` — Postgres/MSSQL expose schemas by returning more `IDatabase`s from
`schemas()`. Every other backend's `schemas()` returns empty; you call
`db->tables()` directly. `execute()` lives on `IDatabase`, not `IConnection`.

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
│   ├── FreeTDS.cmake              — builds db-lib for MSSQL (or uses system libsybdb on Linux)
│   ├── OracleOCI.cmake            — builds ODPI-C
│   └── CassandraDriver.cmake      — builds DataStax C++ driver
├── external/                      — vendored submodules
│   ├── freetds/                   — pinned v1.5.14
│   ├── odpi/                      — pinned v5.6.4
│   └── cassandra-cpp-driver/      — pinned 2.17.1
├── include/dearsql/
│   ├── dearsql.hpp                — umbrella
│   ├── types.hpp / query_result.hpp / connection_info.hpp / database.hpp / factory.hpp
│   └── backends/                  — one header per backend
├── src/
│   ├── types.cpp / connection_info.cpp / factory.cpp
│   ├── sqlite_connection.cpp      — fully implemented
│   └── *_connection.cpp           — skeletons returning "...not implemented yet"
├── docker/
│   └── docker-compose.yml         — 8-service stack for integration tests
├── scripts/
│   └── test-remote                — SSH + tunnel + ctest driver
└── tests/
    ├── test_helpers.hpp           — env loader, tryOpen(), SKIP macros
    ├── common_tests.cpp           — type/helper unit tests (always run)
    ├── sqlite_tests.cpp           — 13 in-memory SQLite tests (always run)
    └── *_tests.cpp                — per-backend; skip when env unset or backend stub
```

## Backend Status

SQLite is fully implemented and covered by 13 in-process tests. Every other
backend has a skeleton that returns `{false, "...not implemented yet"}` from
`open()`. The integration tests for those backends use
`DEARSQL_SKIP_IF_UNIMPLEMENTED` and start asserting automatically once the
backend's body is filled in.

Per-backend test counts (run `./build/tests/dearsql_lib_tests --gtest_list_tests`):

| Backend       | Tests | Notes                                                  |
| ------------- | ----- | ------------------------------------------------------ |
| SQLite        | 13    | full                                                   |
| PostgreSQL    | 16    | + schemas, sequences, routines, materialized views     |
| MySQL         | 13    | + AUTO_INCREMENT detect, routines                      |
| MSSQL         | 10    | + dbo schema, OFFSET/FETCH paging                      |
| Oracle        | 8     | + sequences, FREEPDB1 PDB                              |
| MongoDB       | 7     | + collections-as-tables, JSON command execute          |
| Redis         | 8     | + key types, TTL, SCAN, SELECT db                      |
| Cassandra     | 8     | + keyspace as database, CQL                            |
| Redshift      | 3     | reuses Postgres backend                                |
| Common        | 7     | type/helper unit tests                                 |

## Build

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

## Porting a Backend (the mechanical recipe)

Each `src/<name>_connection.cpp` carries a `// TODO:` comment pointing at the
DearSQL app sources to copy from. The work is mostly subtraction:

1. **Drop async**: every `AsyncOperation<...>` member, every `*Async` method,
   every `start*LoadAsync` / `check*StatusAsync` pair. Replace with direct
   blocking calls.
2. **Drop pooling**: replace `ConnectionPool<T>` with a single connection
   handle behind a `std::mutex` if thread-safety is needed.
3. **Drop UI state**: `attemptedConnection`, `lastConnectionError`,
   `savedConnectionId`, all `*expanded` booleans.
4. **Drop SSH**: every reference to `sshTunnel_`,
   `prepareConnectionForConnect`, `stopSshTunnel`. The app rewrites
   host/port before opening.
5. **Map to interfaces**: backend's two- or three-tier app hierarchy
   (Server → Database[ → Schema]) flattens onto `IConnection`
   (`databases()`/`database()`) and `IDatabase` (`schemas()` for
   Postgres/MSSQL only; otherwise direct `tables()`/`views()`).

The reference implementation is `src/sqlite_connection.cpp`. It shows the
shape: a private `SQLiteDatabaseNode` implementing `IDatabase`, owned by
the `SQLiteConnection`, no schema indirection.

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
- Errors via `Status = pair<bool, string>` for mutations, `QueryResult` for
  queries (`success()` aggregates over multi-statement results).
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

1. **Scope discipline**: this lib is database operations only. If you find
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
