#include "dearsql/backends/redis_connection.hpp"

namespace dearsql {

RedisConnection::RedisConnection(const ConnectionInfo& info) : info_(info) {}
RedisConnection::~RedisConnection() = default;

// TODO: port from src/database/redis.cpp. Drop the AsyncOperation members and
// keep the redisContext + TLS init (redisInitOpenSSL), SELECT db handling,
// SCAN-based key listing, key-group-by-prefix logic, and the CLI-style
// executeQuery parser.
Status RedisConnection::open() {
    return {false, "Redis backend not implemented in libdearsql yet"};
}
void RedisConnection::close() {}
bool RedisConnection::isOpen() const {
    return false;
}
std::vector<DatabasePtr> RedisConnection::databases() {
    return {};
}
DatabasePtr RedisConnection::database(const std::string&) {
    return nullptr;
}
std::vector<RedisKey> RedisConnection::getKeys(const std::string&, int) {
    return {};
}
std::string RedisConnection::getKeyValue(const std::string&, const std::string&) {
    return {};
}
std::string RedisConnection::getKeyType(const std::string&) {
    return {};
}
int64_t RedisConnection::getKeyTTL(const std::string&) {
    return -1;
}
bool RedisConnection::selectDatabase(int idx) {
    selectedDb_ = idx;
    return false;
}

} // namespace dearsql
