#include "dearsql/types.hpp"
#include <format>

namespace dearsql {

void buildForeignKeyLookup(Table& table) {
    table.foreignKeysByColumn.clear();
    table.foreignKeysByColumn.reserve(table.foreignKeys.size());
    for (const auto& fk : table.foreignKeys) {
        if (!fk.sourceColumn.empty()) {
            table.foreignKeysByColumn[fk.sourceColumn] = fk;
        }
    }
}

void populateIncomingForeignKeys(std::vector<Table>& tables) {
    for (auto& t : tables) {
        t.incomingForeignKeys.clear();
    }
    for (const auto& src : tables) {
        for (const auto& fk : src.foreignKeys) {
            if (fk.targetTable.empty())
                continue;
            for (auto& dst : tables) {
                if (dst.name == fk.targetTable) {
                    ForeignKey inc = fk;
                    // mark which table the incoming FK originated from by
                    // re-using the name field's prefix convention
                    if (inc.name.empty()) {
                        inc.name = std::format("from_{}_{}", src.name, fk.sourceColumn);
                    }
                    dst.incomingForeignKeys.push_back(std::move(inc));
                }
            }
        }
    }
}

std::string formatByteSize(int64_t bytes) {
    if (bytes < 0)
        return {};
    constexpr double kKB = 1024.0;
    constexpr double kMB = kKB * 1024.0;
    constexpr double kGB = kMB * 1024.0;
    constexpr double kTB = kGB * 1024.0;
    const double b = static_cast<double>(bytes);
    if (b < kKB)
        return std::format("{} B", bytes);
    if (b < kMB)
        return std::format("{:.1f} KB", b / kKB);
    if (b < kGB)
        return std::format("{:.1f} MB", b / kMB);
    if (b < kTB)
        return std::format("{:.2f} GB", b / kGB);
    return std::format("{:.2f} TB", b / kTB);
}

} // namespace dearsql
