#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

// Oracle backend. A "database" is a schema (owner); each handle runs on its own
// session with CURRENT_SCHEMA set to it, opened on first use. `database()`
// caches one per schema, `openDatabase()` returns a fresh one for host pools.
// Client loading and install: see dearsql/oracle_installer.hpp.
class OracleConnection final : public IConnection {
public:
    explicit OracleConnection(const ConnectionInfo& info);
    ~OracleConnection() override;

    Status open() override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;

    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::ORACLE;
    }
    [[nodiscard]] const ConnectionInfo& info() const override {
        return info_;
    }

    std::vector<DatabasePtr> databases() override;
    DatabasePtr database(const std::string& name = "") override;
    DatabasePtr openDatabase(const std::string& name = "") override;

    // CREATE USER "name" IDENTIFIED BY "name" + GRANT CONNECT, RESOURCE
    Status createDatabase(const CreateDatabaseOptions& opts) override;
    // DROP USER "name" CASCADE
    Status dropDatabase(const std::string& name) override;

private:
    ConnectionInfo info_;
    void* impl_ = nullptr;
};

} // namespace dearsql
