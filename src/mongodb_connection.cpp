#include "dearsql/backends/mongodb_connection.hpp"

namespace dearsql {

MongoDBConnection::MongoDBConnection(const ConnectionInfo& info) : info_(info) {}
MongoDBConnection::~MongoDBConnection() = default;

// TODO: port from src/database/mongodb.cpp + mongodb_database_node.cpp.
// MongoDB exposes "collections" instead of tables. The current app already maps
// collection -> Table, so the porting work is mostly removing the
// AsyncOperation wrappers and the pool plumbing while keeping mongocxx calls.
static Status notImpl() {
    return {false, "MongoDB backend not implemented in libdearsql yet"};
}

Status MongoDBConnection::open() {
    return notImpl();
}
void MongoDBConnection::close() {}
bool MongoDBConnection::isOpen() const {
    return false;
}
std::vector<DatabasePtr> MongoDBConnection::databases() {
    return {};
}
DatabasePtr MongoDBConnection::database(const std::string&) {
    return nullptr;
}
Status MongoDBConnection::dropDatabase(const std::string&) {
    return notImpl();
}

} // namespace dearsql
