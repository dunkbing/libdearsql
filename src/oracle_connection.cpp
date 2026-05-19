#include "dearsql/backends/oracle_connection.hpp"

namespace dearsql {

OracleConnection::OracleConnection(const ConnectionInfo& info) : info_(info) {}
OracleConnection::~OracleConnection() = default;

// TODO: port from src/database/oracle.cpp + oracle/oracle_database_node.cpp.
// Oracle's "schema = user", so listing schemas via ALL_USERS works. Keep
// ODPI-C session pooling and the OFFSET/FETCH NEXT paging. Drop the
// AsyncOperation members.
Status OracleConnection::open() {
    return {false, "Oracle backend not implemented in libdearsql yet"};
}
void OracleConnection::close() {}
bool OracleConnection::isOpen() const {
    return false;
}
std::vector<DatabasePtr> OracleConnection::databases() {
    return {};
}
DatabasePtr OracleConnection::database(const std::string&) {
    return nullptr;
}

} // namespace dearsql
