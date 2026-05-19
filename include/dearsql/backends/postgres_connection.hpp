#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

// PostgreSQL / Redshift backend. Currently a skeleton — open() returns a
// "not implemented" error until the body is ported from the app's PostgresDatabase.
class PostgresConnection final : public IConnection {
public:
    explicit PostgresConnection(const ConnectionInfo& info);
    ~PostgresConnection() override;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return info_.type;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;

    Status createDatabase(const CreateDatabaseOptions& opts) override;
    Status dropDatabase(const std::string& name) override;
    Status renameDatabase(const std::string& oldName, const std::string& newName) override;

private:
    ConnectionInfo info_;
    void* impl_ = nullptr; // opaque (PGconn* once ported)
};

} // namespace dearsql
