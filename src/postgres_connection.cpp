#include "dearsql/backends/postgres_connection.hpp"

namespace dearsql {

PostgresConnection::PostgresConnection(const ConnectionInfo& info) : info_(info) {}
PostgresConnection::~PostgresConnection() = default;

// TODO: port from src/database/postgresql.cpp +
// src/database/postgres/postgres_database_node.cpp + postgres_schema_node.cpp.
// Strip out the async loaders, refresh workflows, and AsyncOperation members —
// the lib only needs the synchronous core. Keep the libpq calls, schema-list
// query, table/view/sequence/routine catalog queries, FK lookups, and the
// table-data pagination logic. SSH handling is intentionally absent (the app
// rewrites host/port before calling open()).
static Status notImpl() {
    return {false, "PostgreSQL backend not implemented in libdearsql yet"};
}

Status PostgresConnection::open() {
    return notImpl();
}
void PostgresConnection::close() {}
bool PostgresConnection::isOpen() const {
    return false;
}

std::vector<DatabasePtr> PostgresConnection::databases() {
    return {};
}
DatabasePtr PostgresConnection::database(const std::string&) {
    return nullptr;
}
Status PostgresConnection::createDatabase(const CreateDatabaseOptions&) {
    return notImpl();
}
Status PostgresConnection::dropDatabase(const std::string&) {
    return notImpl();
}
Status PostgresConnection::renameDatabase(const std::string&, const std::string&) {
    return notImpl();
}

} // namespace dearsql
