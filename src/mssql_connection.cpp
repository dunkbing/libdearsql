#include "dearsql/backends/mssql_connection.hpp"

namespace dearsql {

MSSQLConnection::MSSQLConnection(const ConnectionInfo& info) : info_(info) {}
MSSQLConnection::~MSSQLConnection() = default;

// TODO: port from src/database/mssql.cpp + mssql/mssql_database_node.cpp +
// mssql_schema_node.cpp. MSSQL has both databases AND schemas (like
// "dbo"/"sys"), so the IConnection -> IDatabase -> ISchema hierarchy maps
// cleanly. Keep FreeTDS dblib calls and OFFSET/FETCH NEXT paging.
static Status notImpl() {
    return {false, "MSSQL backend not implemented in libdearsql yet"};
}

Status MSSQLConnection::open() {
    return notImpl();
}
void MSSQLConnection::close() {}
bool MSSQLConnection::isOpen() const {
    return false;
}
std::vector<DatabasePtr> MSSQLConnection::databases() {
    return {};
}
DatabasePtr MSSQLConnection::database(const std::string&) {
    return nullptr;
}
Status MSSQLConnection::createDatabase(const CreateDatabaseOptions&) {
    return notImpl();
}
Status MSSQLConnection::dropDatabase(const std::string&) {
    return notImpl();
}
Status MSSQLConnection::renameDatabase(const std::string&, const std::string&) {
    return notImpl();
}

} // namespace dearsql
