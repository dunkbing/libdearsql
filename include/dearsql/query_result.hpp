#pragma once

#include <string>
#include <vector>

namespace dearsql {

// result of a single SQL statement execution
struct StatementResult {
    bool success = true;
    std::string errorMessage;

    // SELECT
    std::vector<std::string> columnNames;
    std::vector<std::vector<std::string>> tableData;

    // DML
    int affectedRows = 0;

    std::string message;
};

// result of a query execution (may contain multiple statements)
struct QueryResult {
    std::vector<StatementResult> statements;
    double executionTimeMs = 0.0;

    [[nodiscard]] bool success() const {
        if (statements.empty())
            return false;
        for (const auto& s : statements) {
            if (!s.success)
                return false;
        }
        return true;
    }

    [[nodiscard]] const std::string& errorMessage() const {
        static const std::string empty;
        for (const auto& s : statements) {
            if (!s.success)
                return s.errorMessage;
        }
        return empty;
    }

    [[nodiscard]] bool empty() const {
        return statements.empty();
    }
    [[nodiscard]] size_t size() const {
        return statements.size();
    }

    StatementResult& operator[](size_t i) {
        return statements[i];
    }
    const StatementResult& operator[](size_t i) const {
        return statements[i];
    }
};

} // namespace dearsql
