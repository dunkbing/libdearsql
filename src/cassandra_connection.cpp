#include "dearsql/backends/cassandra_connection.hpp"

namespace dearsql {

CassandraConnection::CassandraConnection(const ConnectionInfo& info) : info_(info) {}
CassandraConnection::~CassandraConnection() = default;

// TODO: port from src/database/cassandra.cpp + cassandra/cassandra_database_node.cpp.
// Map IConnection->databases() to "list keyspaces from system_schema.keyspaces".
// CQL has no OFFSET; the SQL builder for Cassandra in the app already handles
// that — bring its LIMIT-only behavior over.
static Status notImpl() {
    return {false, "Cassandra backend not implemented in libdearsql yet"};
}

Status CassandraConnection::open() {
    return notImpl();
}
void CassandraConnection::close() {}
bool CassandraConnection::isOpen() const {
    return false;
}
std::vector<DatabasePtr> CassandraConnection::databases() {
    return {};
}
DatabasePtr CassandraConnection::database(const std::string&) {
    return nullptr;
}
Status CassandraConnection::createDatabase(const CreateDatabaseOptions&) {
    return notImpl();
}
Status CassandraConnection::dropDatabase(const std::string&) {
    return notImpl();
}

} // namespace dearsql
