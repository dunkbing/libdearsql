#include "dearsql/findings.hpp"
#include "dearsql/schema_diff.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace dearsql {

    const std::vector<FindingDoc>& findingDocs() {
        static const std::vector<FindingDoc> docs = {
            {"filtered-full-scan", "A full scan keeps few of the rows it reads",
             "The database reads every row of the table and a filter throws most of them away. "
             "Cost grows with the table, not with the result.",
             "Run the query with EXPLAIN ANALYZE (or count the matching rows) and compare rows "
             "read with rows kept. Check the filtered columns have no usable index.",
             "Index the filtered columns, most selective (or equality) columns first. A partial "
             "index fits a filter that is always the same, such as status = 'open'.",
             "The table is small, the query is rare, or the filter keeps a large share of the "
             "rows: then a scan is the cheaper plan."},
            {"repeated-full-scan", "A table is read in full once per row of a join",
             "On the inner side of a nested loop the table is scanned again for every row of the "
             "outer side, so the work is outer rows x table rows.",
             "Look at the loop count (EXPLAIN ANALYZE) and the join condition on that table.",
             "Index the join columns of the inner table, so each pass becomes a lookup.",
             "The inner table has a handful of rows, or the outer side returns very few."},
            {"sort-spill", "A sort spilled to disk",
             "The rows to sort did not fit in the sort memory, so the database wrote them to "
             "temporary files.",
             "EXPLAIN ANALYZE shows the sort method (external merge) and the space used.",
             "Return fewer rows (LIMIT, a tighter filter), add an index that matches the ORDER "
             "BY, or raise the sort memory (work_mem on PostgreSQL) for this session.",
             "A rare report or batch job where a few extra milliseconds do not matter."},
            {"big-sort", "A large sort",
             "The plan sorts many rows; sorting costs memory and grows faster than the rows.",
             "Check the row count going into the sort and whether the query needs all of them.",
             "An index in ORDER BY order lets the database read rows already sorted, often "
             "with LIMIT stopping early.",
             "The result really needs every row sorted and runs rarely."},
            {"misestimate", "The planner's row estimate is off by 10x or more",
             "The plan was chosen for a different number of rows than the query really "
             "returned, so join order and join methods may be wrong.",
             "Compare estimated and actual rows per step (EXPLAIN ANALYZE); the first step "
             "that is far off is the one to look at.",
             "Refresh statistics (ANALYZE on PostgreSQL and SQLite, ANALYZE TABLE on MySQL). "
             "For correlated columns, PostgreSQL extended statistics can help.",
             "The plan is fast anyway, or the step processes few rows either way."},
            {"automatic-index", "SQLite builds a temporary index on every run",
             "SQLite found no index for the join and builds one for the duration of the query, "
             "then throws it away.",
             "EXPLAIN QUERY PLAN shows USING AUTOMATIC INDEX on that table.",
             "CREATE INDEX on the columns SQLite indexed, so it is built once.",
             "A one-off query, or a table you never query this way again."},
            {"unindexed-fk", "A foreign key has no index",
             "Joins on this key, and deletes or updates of the referenced row (which must find "
             "the referencing rows), scan the whole table.",
             "Check that no index starts with the key's columns; look at how often the parent "
             "rows are deleted or the tables joined.",
             "CREATE INDEX on the foreign key columns, in the key's order.",
             "The table is small, the parent rows never change and the key is never joined."},
            {"redundant-index", "An index is covered by another",
             "Its columns are the same as, or lead, another index's, which can serve the same "
             "lookups. Every write still updates both.",
             "Make sure no query, constraint or foreign key relies on this exact index, and "
             "that the wider index has the columns in the same order.",
             "DROP INDEX the narrower one (after checking).",
             "It enforces uniqueness the other does not, or a hint or tool names it."},
        };
        return docs;
    }

    const FindingDoc* findingDoc(std::string_view code) {
        for (const auto& d : findingDocs()) {
            if (d.code == code) {
                return &d;
            }
        }
        return nullptr;
    }

    namespace {
        std::string joined(const std::vector<std::string>& cols) {
            std::string out;
            for (const auto& c : cols) {
                out += (out.empty() ? "" : ", ") + c;
            }
            return out;
        }

        bool sameSet(std::vector<std::string> a, std::vector<std::string> b) {
            std::ranges::sort(a);
            std::ranges::sort(b);
            return a == b;
        }
    } // namespace

    std::vector<IndexFinding> reviewIndexes(const std::vector<Table>& tables, DatabaseType type) {
        std::vector<IndexFinding> out;
        for (const auto& t : tables) {
            const std::string table = t.schema.empty() ? t.name : t.schema + "." + t.name;
            // every index as an ordered column list; the primary key counts even when the
            // engine does not list it as an index (sqlite rowid tables)
            std::vector<const Index*> indexes;
            for (const auto& i : t.indexes) {
                indexes.push_back(&i);
            }
            Index pk{.name = "PRIMARY KEY", .isUnique = true, .isPrimary = true};
            for (const auto& c : t.columns) {
                if (c.isPrimaryKey) {
                    pk.columns.push_back(c.name);
                }
            }
            if (!pk.columns.empty() &&
                std::ranges::none_of(indexes, [](const Index* i) { return i->isPrimary; })) {
                indexes.push_back(&pk);
            }

            // foreign keys: one row per column, grouped by constraint name
            std::map<std::string, std::vector<std::string>> fks;
            std::map<std::string, std::string> targets;
            for (const auto& fk : t.foreignKeys) {
                const std::string key = fk.name.empty() ? fk.sourceColumn : fk.name;
                fks[key].push_back(fk.sourceColumn);
                targets[key] = fk.targetTable;
            }
            for (const auto& [name, cols] : fks) {
                const bool covered = std::ranges::any_of(indexes, [&](const Index* i) {
                    if (i->columns.size() < cols.size()) {
                        return false;
                    }
                    return sameSet({i->columns.begin(), i->columns.begin() + cols.size()}, cols);
                });
                if (covered) {
                    continue;
                }
                std::string idx = "idx_" + t.name;
                for (const auto& c : cols) {
                    idx += "_" + c;
                }
                out.push_back({"unindexed-fk", table,
                               std::format("{} ({}) references {} but no index starts with {}.",
                                           table, joined(cols), targets[name],
                                           cols.size() == 1 ? "it" : "those columns"),
                               std::format("CREATE INDEX {} ON {} ({});", idx, table,
                                           joined(cols))});
            }

            // an index whose columns lead another's
            std::set<std::string> reported;
            // the index to keep is named first: primary, then unique, then by name
            auto keepers = indexes;
            std::ranges::sort(keepers, [](const Index* x, const Index* y) {
                if (x->isPrimary != y->isPrimary) {
                    return x->isPrimary;
                }
                if (x->isUnique != y->isUnique) {
                    return x->isUnique;
                }
                return x->name < y->name;
            });
            for (const Index* a : indexes) {
                if (a->isPrimary || a->columns.empty()) {
                    continue;
                }
                for (const Index* b : keepers) {
                    if (a == b || b->columns.size() < a->columns.size() ||
                        !std::equal(a->columns.begin(), a->columns.end(), b->columns.begin())) {
                        continue;
                    }
                    const bool same = a->columns.size() == b->columns.size();
                    // a unique index is only replaced by one unique on the same columns
                    if (a->isUnique && !(same && b->isUnique)) {
                        continue;
                    }
                    // exact duplicates: keep the primary/unique one, else the first by name
                    if (same && !b->isPrimary && !(b->isUnique && !a->isUnique) &&
                        b->name > a->name) {
                        continue;
                    }
                    if (!reported.insert(a->name).second) {
                        break;
                    }
                    out.push_back(
                        {"redundant-index", table,
                         same ? std::format("{}: index {} ({}) duplicates {}.", table, a->name,
                                            joined(a->columns), b->name)
                              : std::format("{}: index {} ({}) leads {} ({}), which serves the "
                                            "same lookups.",
                                            table, a->name, joined(a->columns), b->name,
                                            joined(b->columns)),
                         dropIndexSql(type, a->name, table)});
                    break;
                }
            }
        }
        return out;
    }

} // namespace dearsql
