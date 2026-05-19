#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

struct RedisKey {
    std::string name;
    std::string type;
    std::string value;
    int64_t ttl = -1;
    int64_t size = -1;
};

struct RedisDbInfo {
    int index = 0;
    int64_t keys = 0;
    int64_t expires = 0;
    int64_t avgTtl = 0;
    bool hasKeys = false;
};

class RedisConnection final : public IConnection {
public:
    explicit RedisConnection(const ConnectionInfo& info);
    ~RedisConnection() override;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::REDIS;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    // For Redis, databases() returns one handle per Redis logical DB index
    // (0..N-1 based on server config). Each handle's name() is the index.
    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;

    // Redis-specific helpers (porting these comes next).
    std::vector<RedisKey> getKeys(const std::string& pattern = "*", int limit = 1000);
    std::string getKeyValue(const std::string& key, const std::string& knownType = "");
    std::string getKeyType(const std::string& key);
    int64_t getKeyTTL(const std::string& key);
    bool selectDatabase(int dbIndex);

private:
    ConnectionInfo info_;
    void* impl_ = nullptr;
    int selectedDb_ = 0;
};

} // namespace dearsql
