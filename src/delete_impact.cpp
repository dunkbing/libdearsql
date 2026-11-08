#include "dearsql/delete_impact.hpp"

#include "dearsql/sql_builder.hpp"

#include <format>
#include <map>

namespace dearsql {

    namespace {

        struct Level {
            const Table* table;
            std::vector<std::pair<std::string, std::string>> join; // child column, parent column
        };

        // rows of chain.back() reached from the deleted rows, as a condition on alias t<n>.
        // aliases without AS: oracle rejects "FROM t AS a"
        std::string reached(const ISQLBuilder& b, const std::vector<Level>& chain, size_t n,
                            const std::string& rootWhere) {
            if (n == 0) {
                return "(" + rootWhere + ")";
            }
            std::string on;
            for (const auto& [child, parent] : chain[n].join) {
                on += std::format(" AND t{}.{} = t{}.{}", n - 1, b.quoteIdentifier(parent), n,
                                  b.quoteIdentifier(child));
            }
            return std::format("EXISTS (SELECT 1 FROM {} t{} WHERE {}{})",
                               b.qualifiedName(*chain[n - 1].table), n - 1,
                               reached(b, chain, n - 1, rootWhere), on);
        }

        void walk(const ISQLBuilder& b, const std::vector<Table>& tables, std::vector<Level>& chain,
                  const std::string& rootWhere, int maxDepth, const std::string& path,
                  std::vector<DeleteEffect>& out) {
            const Table& parent = *chain.back().table;
            for (const auto& child : tables) {
                // one entry per column pair: group a composite key by its name
                std::map<std::string, std::vector<const ForeignKey*>> keys;
                for (const auto& fk : child.foreignKeys) {
                    if (fk.targetTable == parent.name) {
                        keys[fk.name].push_back(&fk);
                    }
                }
                for (const auto& [name, parts] : keys) {
                    Level level{&child, {}};
                    std::string columns;
                    for (const auto* fk : parts) {
                        level.join.emplace_back(fk->sourceColumn, fk->targetColumn);
                        columns += (columns.empty() ? "" : ", ") + fk->sourceColumn;
                    }
                    chain.push_back(std::move(level));
                    const size_t n = chain.size() - 1;
                    DeleteEffect e;
                    e.table = child.name;
                    e.columns = columns;
                    e.action =
                        parts.front()->onDelete.empty() ? "NO ACTION" : parts.front()->onDelete;
                    e.path = path + " > " + child.name;
                    e.depth = static_cast<int>(n);
                    e.countSql =
                        std::format("SELECT COUNT(*) FROM {} t{} WHERE {}", b.qualifiedName(child),
                                    n, reached(b, chain, n, rootWhere));
                    out.push_back(e);
                    if (e.action == "CASCADE" && e.depth < maxDepth) {
                        walk(b, tables, chain, rootWhere, maxDepth, e.path, out);
                    }
                    chain.pop_back();
                }
            }
        }

    } // namespace

    std::vector<DeleteEffect> deleteEffects(DatabaseType type, const std::vector<Table>& tables,
                                            const Table& root, const std::string& rootWhere,
                                            int maxDepth) {
        std::vector<DeleteEffect> out;
        const auto builder = createSQLBuilder(type);
        if (!builder || rootWhere.empty()) {
            return out;
        }
        std::vector<Level> chain{{&root, {}}};
        walk(*builder, tables, chain, rootWhere, maxDepth, root.name, out);
        return out;
    }

} // namespace dearsql
