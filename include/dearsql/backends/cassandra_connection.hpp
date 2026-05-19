#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

// Cassandra calls databases "keyspaces"; we expose them via the standard
// IConnection::databases()/database() API for consistency.
class CassandraConnection final : public IConnection {
public:
    explicit CassandraConnection(const ConnectionInfo& info);
    ~CassandraConnection() override;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::CASSANDRA;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;

    Status createDatabase(const CreateDatabaseOptions& opts) override;
    Status dropDatabase(const std::string& name) override;

private:
    ConnectionInfo info_;
};

} // namespace dearsql
