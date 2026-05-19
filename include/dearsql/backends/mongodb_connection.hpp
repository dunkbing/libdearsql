#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

class MongoDBConnection final : public IConnection {
public:
    explicit MongoDBConnection(const ConnectionInfo& info);
    ~MongoDBConnection() override;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::MONGODB;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;
    Status dropDatabase(const std::string& name) override;

private:
    ConnectionInfo info_;
    void* impl_ = nullptr;
};

} // namespace dearsql
