#pragma once

#include "dearsql/database.hpp"

namespace dearsql {

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

private:
    ConnectionInfo info_;
};

} // namespace dearsql
