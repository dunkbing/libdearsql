#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

class MSSQLConnection final : public IConnection {
public:
    explicit MSSQLConnection(const ConnectionInfo& info);
    ~MSSQLConnection() override;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::MSSQL;
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
};

} // namespace dearsql
