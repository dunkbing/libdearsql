#include "dearsql/types.hpp"
#include <format>

namespace dearsql {

void buildForeignKeyLookup(Table& table) {
    table.foreignKeysByColumn.clear();
    for (const auto& fk : table.foreignKeys)
        table.foreignKeysByColumn[fk.sourceColumn] = fk;
}

void populateIncomingForeignKeys(std::vector<Table>& tables) {
    std::unordered_map<std::string, Table*> lookup;
    lookup.reserve(tables.size());
    for (auto& t : tables) {
        t.incomingForeignKeys.clear();
        lookup[t.name] = &t;
    }
    for (const auto& src : tables) {
        for (const auto& fk : src.foreignKeys) {
            auto it = lookup.find(fk.targetTable);
            if (it == lookup.end())
                continue;
            ForeignKey incoming = fk;
            incoming.targetTable = src.name; // the table referencing us
            it->second->incomingForeignKeys.push_back(std::move(incoming));
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
