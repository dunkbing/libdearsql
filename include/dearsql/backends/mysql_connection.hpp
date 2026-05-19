#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

// MySQL / MariaDB backend.
class MySQLConnection final : public IConnection {
public:
    explicit MySQLConnection(const ConnectionInfo& info);
    ~MySQLConnection() override;

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
    void* impl_ = nullptr;
};

} // namespace dearsql
