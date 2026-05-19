#include "dearsql/backends/mysql_connection.hpp"

namespace dearsql {

MySQLConnection::MySQLConnection(const ConnectionInfo& info) : info_(info) {}
MySQLConnection::~MySQLConnection() = default;

// TODO: port from src/database/mysql.cpp + src/database/mysql/mysql_database_node.cpp.
// Use a single MYSQL* per connection plus a mutex; drop ConnectionPool. Keep
// SHOW DATABASES, INFORMATION_SCHEMA queries for columns/indexes/FKs/routines,
// and the table-data + row-count logic. SSL mode mapping (MYSQL_OPT_SSL_MODE)
// stays.
static Status notImpl() {
    return {false, "MySQL backend not implemented in libdearsql yet"};
}

Status MySQLConnection::open() {
    return notImpl();
}
void MySQLConnection::close() {}
bool MySQLConnection::isOpen() const {
    return false;
}
std::vector<DatabasePtr> MySQLConnection::databases() {
    return {};
}
DatabasePtr MySQLConnection::database(const std::string&) {
    return nullptr;
}
Status MySQLConnection::createDatabase(const CreateDatabaseOptions&) {
    return notImpl();
}
Status MySQLConnection::dropDatabase(const std::string&) {
    return notImpl();
}
Status MySQLConnection::renameDatabase(const std::string&, const std::string&) {
    return notImpl();
}

} // namespace dearsql
