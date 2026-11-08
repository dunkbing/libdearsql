#include "dearsql/profile.hpp"

#include "dearsql/ddl_utils.hpp"
#include "dearsql/sql_builder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <string_view>

namespace dearsql {

    namespace {
        constexpr size_t kMaxColumns = 64;

        bool sqlEngine(DatabaseType type) {
            return type != DatabaseType::MONGODB && type != DatabaseType::REDIS &&
                   !isCassandraFamily(type) && type != DatabaseType::S3;
        }

        std::string lower(std::string_view s) {
            std::string out(s);
            for (auto& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        bool hasAny(const std::string& s, std::initializer_list<std::string_view> words) {
            return std::ranges::any_of(words, [&](auto w) { return s.find(w) != s.npos; });
        }

        // equality works (COUNT(DISTINCT), GROUP BY): not json (jsonb is fine), lobs, xml,
        // spatial, or sql server's old text types
        bool comparable(DatabaseType type, const std::string& rawType) {
            const std::string t = lower(rawType);
            if (t.find("jsonb") != t.npos)
                return true;
            if (hasAny(t, {"json", "xml", "clob", "blob", "image", "geometry", "geography", "point",
                           "polygon", "box", "circle", "path", "lseg"}))
                return false;
            return !(type == DatabaseType::MSSQL && (t == "text" || t == "ntext"));
        }

        // MIN/MAX work: numbers, dates and strings; not bool, uuid, json, binary or bit
        bool ordered(DatabaseType type, const std::string& rawType) {
            const std::string t = lower(rawType);
            if (!comparable(type, rawType) ||
                hasAny(t, {"bool", "uuid", "json", "bytea", "binary", "bit", "[]", "array",
                           "interval", "enum", "set("}))
                return false;
            return hasAny(t, {"int", "serial", "numeric", "decimal", "number", "real", "double",
                              "float", "money", "date", "time", "year", "char", "text", "string"});
        }

        // the first n rows of `select` from the table
        std::string sampleSelect(DatabaseType type, const ISQLBuilder& b, const Table& table,
                                 const std::string& select, int n) {
            const std::string from = b.qualifiedName(table);
            if (type == DatabaseType::MSSQL)
                return std::format("SELECT TOP {} {} FROM {}", n, select, from);
            if (type == DatabaseType::ORACLE)
                return std::format("SELECT {} FROM {} WHERE ROWNUM <= {}", select, from, n);
            return std::format("SELECT {} FROM {} LIMIT {}", select, from, n);
        }
        // the same, as a derived table
        std::string sampleOf(DatabaseType type, const ISQLBuilder& b, const Table& table,
                             const std::string& select, int n) {
            return "(" + sampleSelect(type, b, table, select, n) + ") s";
        }

        std::string textType(DatabaseType type) {
            if (isMySQLFamily(type))
                return "CHAR";
            if (type == DatabaseType::MSSQL)
                return "NVARCHAR(4000)";
            if (type == DatabaseType::ORACLE)
                return "VARCHAR2(4000)";
            return "TEXT";
        }

        std::vector<const Column*> profiled(const Table& table) {
            std::vector<const Column*> out;
            for (const auto& c : table.columns) {
                if (out.size() == kMaxColumns)
                    break;
                out.push_back(&c);
            }
            return out;
        }

        int64_t toInt(const std::string& s) {
            try {
                return std::stoll(s);
            } catch (...) {
                return 0;
            }
        }
    } // namespace

    std::optional<std::string> profileSql(DatabaseType type, const Table& table, int sample) {
        const auto cols = profiled(table);
        if (!sqlEngine(type) || cols.empty())
            return std::nullopt;
        const auto b = createSQLBuilder(type);
        std::string select, aggregates = "COUNT(*)";
        for (const Column* c : cols) {
            const std::string q = b->quoteIdentifier(c->name);
            select += (select.empty() ? "" : ", ") + q;
            aggregates += ", COUNT(" + q + ")";
            if (comparable(type, c->type))
                aggregates += ", COUNT(DISTINCT " + q + ")";
            if (ordered(type, c->type))
                aggregates += std::format(", MIN({0}), MAX({0})", q);
        }
        return std::format("SELECT {} FROM {}", aggregates,
                           sampleOf(type, *b, table, select, sample));
    }

    std::optional<std::string> profileRowsSql(DatabaseType type, const Table& table, int sample) {
        const auto cols = profiled(table);
        if (!sqlEngine(type) || cols.empty())
            return std::nullopt;
        const auto b = createSQLBuilder(type);
        // aggregates once over the sample (a), then one row per column from it
        std::string select, aggregates = "COUNT(*) AS n_rows";
        std::string rows;
        auto text = [&](const std::string& expr) {
            return std::format("CAST({} AS {})", expr, textType(type));
        };
        for (size_t i = 0; i < cols.size(); ++i) {
            const Column* c = cols[i];
            const std::string q = b->quoteIdentifier(c->name);
            select += (select.empty() ? "" : ", ") + q;
            aggregates += std::format(", COUNT({}) AS nn_{}", q, i);
            const bool distinct = comparable(type, c->type);
            const bool minMax = ordered(type, c->type);
            if (distinct)
                aggregates += std::format(", COUNT(DISTINCT {}) AS d_{}", q, i);
            if (minMax)
                aggregates += std::format(", MIN({0}) AS mn_{1}, MAX({0}) AS mx_{1}", q, i);
            rows += std::format(
                "{}SELECT '{}' AS {}, '{}' AS {}, n_rows - nn_{} AS {}, {} AS {}, {} AS {}, "
                "{} AS {}, n_rows AS {} FROM a",
                rows.empty() ? "" : "\nUNION ALL ", ddl_utils::escapeSingleQuotes(c->name),
                b->quoteIdentifier("column"), ddl_utils::escapeSingleQuotes(c->type),
                b->quoteIdentifier("type"), i, b->quoteIdentifier("nulls"),
                distinct ? std::format("d_{}", i) : "NULL", b->quoteIdentifier("distinct"),
                minMax ? text(std::format("mn_{}", i)) : text("NULL"), b->quoteIdentifier("min"),
                minMax ? text(std::format("mx_{}", i)) : text("NULL"), b->quoteIdentifier("max"),
                b->quoteIdentifier("rows"));
        }
        return std::format("-- first {} rows of {}, not a random sample\n"
                           "WITH s AS ({}),\na AS (SELECT {} FROM s)\n{}",
                           sample, b->qualifiedName(table),
                           sampleSelect(type, *b, table, select, sample), aggregates, rows);
    }

    std::optional<TableProfile> parseProfile(DatabaseType type, const Table& table,
                                             const StatementResult& result, int sample) {
        if (!result.success || result.tableData.empty())
            return std::nullopt;
        const auto& row = result.tableData.front();
        size_t at = 0;
        auto next = [&]() -> std::string {
            return at < row.size() ? row[at++] : std::string(NULL_SENTINEL);
        };
        auto value = [](const std::string& v) { return isNullSentinel(v) ? std::string() : v; };
        TableProfile p;
        p.rows = toInt(next());
        p.sampled = p.rows >= sample;
        for (const Column* c : profiled(table)) {
            ColumnProfile cp{.column = c->name, .type = c->type};
            cp.nonNull = toInt(next());
            if (comparable(type, c->type))
                cp.distinct = toInt(next());
            if (ordered(type, c->type)) {
                cp.min = value(next());
                cp.max = value(next());
                cp.hasMinMax = true;
            }
            p.columns.push_back(std::move(cp));
        }
        return p;
    }

    std::optional<std::string> topValuesSql(DatabaseType type, const Table& table,
                                            const Column& column, int limit, int sample) {
        if (!sqlEngine(type) || !comparable(type, column.type))
            return std::nullopt;
        const auto b = createSQLBuilder(type);
        const std::string q = b->quoteIdentifier(column.name);
        const std::string grouped =
            std::format("{0}, COUNT(*) AS n FROM {1} GROUP BY {0} ORDER BY COUNT(*) DESC", q,
                        sampleOf(type, *b, table, q, sample));
        if (type == DatabaseType::MSSQL)
            return std::format("SELECT TOP {} {}", limit, grouped);
        if (type == DatabaseType::ORACLE)
            return std::format("SELECT * FROM (SELECT {}) WHERE ROWNUM <= {}", grouped, limit);
        return std::format("SELECT {} LIMIT {}", grouped, limit);
    }

} // namespace dearsql
