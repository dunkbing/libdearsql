#include "dearsql/schema_diff.hpp"

#include "dearsql/sql_builder.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <map>

namespace dearsql {

    namespace {
        // "a; b" (alterColumn joins that way) -> one ';'-terminated statement per line
        std::string terminated(std::string sql) {
            for (size_t at = 0; (at = sql.find("; ", at)) != std::string::npos;)
                sql.replace(at, 2, ";\n");
            while (!sql.empty() && std::isspace(static_cast<unsigned char>(sql.back())))
                sql.pop_back();
            if (!sql.empty() && sql.back() != ';')
                sql += ';';
            return sql;
        }

        std::string normalType(const std::string& type) {
            std::string out;
            for (const char c : type) {
                if (!std::isspace(static_cast<unsigned char>(c)))
                    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return out;
        }

        std::string joined(const std::vector<std::string>& names) {
            std::string out;
            for (const auto& n : names)
                out += (out.empty() ? "" : ", ") + n;
            return out;
        }

        // an index by what it does, not its name
        std::string signature(const Index& i) {
            return (i.isUnique ? "u:" : ":") + joined(i.columns);
        }

        // composite foreign keys come one row per column: one group per constraint
        struct Fk {
            std::string name;
            std::vector<std::string> from, to;
            std::string target, onDelete, onUpdate;
            [[nodiscard]] std::string signature() const {
                return joined(from) + ">" + target + "(" + joined(to) + ")";
            }
        };
        std::vector<Fk> foreignKeys(const Table& t) {
            std::vector<Fk> out;
            for (const auto& fk : t.foreignKeys) {
                auto it = std::ranges::find_if(
                    out, [&](const Fk& g) { return !fk.name.empty() && g.name == fk.name; });
                if (it == out.end()) {
                    out.push_back({fk.name, {}, {}, fk.targetTable, fk.onDelete, fk.onUpdate});
                    it = std::prev(out.end());
                }
                it->from.push_back(fk.sourceColumn);
                it->to.push_back(fk.targetColumn);
            }
            return out;
        }

        const Table* find(const std::vector<Table>& tables, const std::string& name) {
            const auto it = std::ranges::find(tables, name, &Table::name);
            return it == tables.end() ? nullptr : &*it;
        }
    } // namespace

    bool sameDialect(DatabaseType a, DatabaseType b) {
        return a == b || (isPostgresFamily(a) && isPostgresFamily(b)) ||
               (isMySQLFamily(a) && isMySQLFamily(b));
    }

    std::string dropIndexSql(DatabaseType type, const std::string& index,
                             const std::string& table) {
        if (isMySQLFamily(type) || type == DatabaseType::MSSQL)
            return std::format("DROP INDEX {} ON {};", index, table);
        return std::format("DROP INDEX {};", index);
    }

    std::vector<SchemaChange> diffSchemas(const std::vector<Table>& fromAll,
                                          const std::vector<Table>& toAll, DatabaseType type,
                                          const std::string& schema) {
        using Kind = SchemaChange::Kind;
        const auto builder = createSQLBuilder(type);
        const bool sqlite = type == DatabaseType::SQLITE;
        auto q = [&](const std::string& id) { return builder->quoteIdentifier(id); };
        // views are left out: their bodies would need their own comparison
        std::vector<Table> from, to;
        std::ranges::copy_if(fromAll, std::back_inserter(from),
                             [](const Table& t) { return t.definition.empty(); });
        std::ranges::copy_if(toAll, std::back_inserter(to),
                             [](const Table& t) { return t.definition.empty(); });

        std::vector<SchemaChange> out;
        auto add = [&](Kind kind, const std::string& table, std::string message,
                       const std::string& sql) {
            out.push_back({kind, table, std::move(message), sql.empty() ? "" : terminated(sql)});
        };

        for (const auto& t : to) {
            const Table* f = find(from, t.name);
            if (!f) {
                Table created = t;
                created.schema = schema;
                add(Kind::CreateTable, t.name,
                    std::format("{}: new table ({} columns)", t.name, t.columns.size()),
                    builder->tableDdl(created, schema));
                continue;
            }
            const std::string table = builder->qualifiedName(*f);

            for (const auto& c : t.columns) {
                const auto it = std::ranges::find(f->columns, c.name, &Column::name);
                if (it == f->columns.end()) {
                    const bool risky = c.isNotNull && c.defaultValue.empty();
                    add(Kind::AddColumn, t.name,
                        std::format("{}: add column {} {}{}", t.name, c.name, c.type,
                                    risky ? " NOT NULL (no default: fails if the table has rows)"
                                          : ""),
                        builder->addColumn(table, c));
                    continue;
                }
                std::vector<std::string> what;
                if (normalType(it->type) != normalType(c.type))
                    what.push_back(std::format("type {} -> {}", it->type, c.type));
                if (it->isNotNull != c.isNotNull)
                    what.push_back(c.isNotNull ? "NOT NULL" : "nullable");
                if (it->defaultValue != c.defaultValue)
                    what.push_back(std::format("default {} -> {}",
                                               it->defaultValue.empty() ? "none" : it->defaultValue,
                                               c.defaultValue.empty() ? "none" : c.defaultValue));
                const bool alter = !what.empty();
                if (it->isPrimaryKey != c.isPrimaryKey)
                    what.push_back(c.isPrimaryKey ? "joins the primary key"
                                                  : "leaves the primary key");
                if (what.empty())
                    continue;
                std::string message = std::format("{}.{}: {}", t.name, c.name, joined(what));
                std::string sql = alter ? builder->alterColumn(table, it->name, c) : "";
                if (sql.starts_with("--")) // sqlite answers with a comment
                    sql.clear();
                if (alter && sql.empty())
                    message += sqlite ? " (SQLite: rebuild the table to change a column)"
                                      : " (this engine cannot change a column in place)";
                else if (!alter)
                    message += " (change the primary key by hand)";
                add(Kind::AlterColumn, t.name, std::move(message), sql);
            }
            for (const auto& c : f->columns) {
                if (std::ranges::find(t.columns, c.name, &Column::name) == t.columns.end())
                    add(Kind::DropColumn, t.name,
                        std::format("{}: drop column {} (and its data)", t.name, c.name),
                        builder->dropColumn(table, c.name));
            }

            // a dropped index's name, qualified where postgres keeps it (the table's schema)
            auto indexRef = [&](const std::string& name) {
                return isPostgresFamily(type) && !f->schema.empty() ? q(f->schema) + "." + q(name)
                                                                    : q(name);
            };
            std::vector<std::string> replaced; // dropped as part of an add of the same name
            for (const auto& i : t.indexes) {
                if (i.isPrimary || i.columns.empty() ||
                    std::ranges::any_of(f->indexes, [&](const Index& x) {
                        return !x.isPrimary && signature(x) == signature(i);
                    }))
                    continue;
                std::string cols;
                for (const auto& c : i.columns)
                    cols += (cols.empty() ? "" : ", ") + q(c);
                std::string sql = std::format("CREATE {}INDEX {} ON {} ({});",
                                              i.isUnique ? "UNIQUE " : "", q(i.name), table, cols);
                // same name, other columns: drop it first, or the create collides
                if (std::ranges::any_of(f->indexes,
                                        [&](const Index& x) { return x.name == i.name; })) {
                    sql = dropIndexSql(type, indexRef(i.name), table) + "\n" + sql;
                    replaced.push_back(i.name);
                }
                add(Kind::AddIndex, t.name,
                    std::format("{}: add {}index {} ({})", t.name, i.isUnique ? "unique " : "",
                                i.name, joined(i.columns)),
                    sql);
            }
            for (const auto& i : f->indexes) {
                if (i.isPrimary || i.columns.empty() ||
                    std::ranges::find(replaced, i.name) != replaced.end() ||
                    std::ranges::any_of(t.indexes, [&](const Index& x) {
                        return !x.isPrimary && signature(x) == signature(i);
                    }))
                    continue;
                add(Kind::DropIndex, t.name,
                    std::format("{}: drop index {} ({})", t.name, i.name, joined(i.columns)),
                    dropIndexSql(type, indexRef(i.name), table));
            }

            const auto fromKeys = foreignKeys(*f);
            const auto toKeys = foreignKeys(t);
            for (const auto& k : toKeys) {
                if (std::ranges::any_of(
                        fromKeys, [&](const Fk& x) { return x.signature() == k.signature(); }))
                    continue;
                std::string message = std::format("{}: add foreign key ({}) -> {} ({})", t.name,
                                                  joined(k.from), k.target, joined(k.to));
                std::string sql;
                if (sqlite) {
                    message += " (SQLite: rebuild the table to add a foreign key)";
                } else {
                    std::string from, refs;
                    for (const auto& c : k.from)
                        from += (from.empty() ? "" : ", ") + q(c);
                    for (const auto& c : k.to)
                        refs += (refs.empty() ? "" : ", ") + q(c);
                    const std::string target =
                        f->schema.empty() ? q(k.target) : q(f->schema) + "." + q(k.target);
                    sql = std::format("ALTER TABLE {} ADD {}FOREIGN KEY ({}) REFERENCES {} ({})",
                                      table, k.name.empty() ? "" : "CONSTRAINT " + q(k.name) + " ",
                                      from, target, refs);
                    if (!k.onDelete.empty() && k.onDelete != "NO ACTION")
                        sql += " ON DELETE " + k.onDelete;
                    if (!k.onUpdate.empty() && k.onUpdate != "NO ACTION")
                        sql += " ON UPDATE " + k.onUpdate;
                }
                add(Kind::AddForeignKey, t.name, std::move(message), sql);
            }
            for (const auto& k : fromKeys) {
                if (std::ranges::any_of(
                        toKeys, [&](const Fk& x) { return x.signature() == k.signature(); }))
                    continue;
                std::string message = std::format("{}: drop foreign key ({}) -> {} ({})", t.name,
                                                  joined(k.from), k.target, joined(k.to));
                std::string sql;
                if (sqlite || k.name.empty())
                    message += sqlite ? " (SQLite: rebuild the table to drop a foreign key)"
                                      : " (unnamed: drop it by hand)";
                else
                    sql =
                        std::format("ALTER TABLE {} DROP {} {}", table,
                                    isMySQLFamily(type) ? "FOREIGN KEY" : "CONSTRAINT", q(k.name));
                add(Kind::DropForeignKey, t.name, std::move(message), sql);
            }
        }

        for (const auto& f : from) {
            if (!find(to, f.name))
                add(Kind::DropTable, f.name, std::format("{}: drop table (and its data)", f.name),
                    builder->dropTable(f.schema, f.name));
        }

        std::ranges::stable_sort(out, {}, &SchemaChange::kind);
        return out;
    }

} // namespace dearsql
