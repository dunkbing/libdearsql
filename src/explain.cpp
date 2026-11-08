#include "dearsql/explain.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <regex>
#include <sstream>

namespace dearsql {

    namespace {

        using json = nlohmann::json;

        std::string trimStatement(std::string_view sql) {
            size_t b = 0;
            size_t e = sql.size();
            while (b < e && std::isspace(static_cast<unsigned char>(sql[b]))) {
                ++b;
            }
            while (e > b &&
                   (std::isspace(static_cast<unsigned char>(sql[e - 1])) || sql[e - 1] == ';')) {
                --e;
            }
            return std::string(sql.substr(b, e - b));
        }

        // first column of every row, one per line: how each engine returns its plan text
        std::string firstColumn(const StatementResult& r) {
            std::string out;
            for (const auto& row : r.tableData) {
                if (!row.empty()) {
                    if (!out.empty()) {
                        out += '\n';
                    }
                    out += row.front();
                }
            }
            return out;
        }

        // children first: a node's own time is its total minus what its children took
        void fillSelfTimes(PlanNode& node) {
            double childTotal = 0.0;
            for (auto& child : node.children) {
                fillSelfTimes(child);
                childTotal += child.totalTimeMs.value_or(0.0);
            }
            if (node.totalTimeMs) {
                node.selfTimeMs = std::max(0.0, *node.totalTimeMs - childTotal);
            }
        }

        // ---------------------------------------------------------------- postgres

        std::string scalarText(const json& v) {
            if (v.is_string()) {
                return v.get<std::string>();
            }
            if (v.is_array()) {
                std::string out;
                for (const auto& item : v) {
                    if (!out.empty()) {
                        out += ", ";
                    }
                    out += scalarText(item);
                }
                return out;
            }
            if (v.is_number_float()) {
                std::ostringstream s;
                s << v.get<double>();
                return s.str();
            }
            return v.dump();
        }

        std::optional<double> number(const json& node, const char* key) {
            const auto it = node.find(key);
            if (it == node.end() || !it->is_number()) {
                return std::nullopt;
            }
            return it->get<double>();
        }

        // under a Gather the workers run their loops at the same time: per-loop time is wall time
        PlanNode postgresNode(const json& j, bool parallel = false) {
            PlanNode node;
            std::string op = j.value("Node Type", "?");
            // as postgres's own text output names them: "Hash Left Join", "HashAggregate"
            const std::string joinType = j.value("Join Type", "");
            if (!joinType.empty() && joinType != "Inner") {
                const auto pos = op.find(" Join");
                op = pos == std::string::npos ? op + " " + joinType + " Join"
                                              : op.substr(0, pos) + " " + joinType + op.substr(pos);
            }
            if (op == "Aggregate") {
                const std::string strategy = j.value("Strategy", "");
                if (strategy == "Hashed") {
                    op = "HashAggregate";
                } else if (strategy == "Sorted") {
                    op = "GroupAggregate";
                }
            }
            if (j.value("Parallel Aware", false)) {
                op = "Parallel " + op;
            }
            node.operation = op;

            std::string detail;
            if (const auto rel = j.value("Relation Name", ""); !rel.empty()) {
                detail = "on " + rel;
                if (const auto alias = j.value("Alias", ""); !alias.empty() && alias != rel) {
                    detail += " " + alias;
                }
            } else if (const auto cte = j.value("CTE Name", ""); !cte.empty()) {
                detail = "on " + cte;
            } else if (const auto fn = j.value("Function Name", ""); !fn.empty()) {
                detail = "on " + fn + "()";
            }
            if (const auto index = j.value("Index Name", ""); !index.empty()) {
                detail += (detail.empty() ? "using " : " using ") + index;
            }
            node.detail = detail;

            node.estimatedRows = number(j, "Plan Rows");
            node.cost = number(j, "Total Cost");
            node.actualRows = number(j, "Actual Rows");
            node.loops = number(j, "Actual Loops");
            if (const auto perLoop = number(j, "Actual Total Time")) {
                node.totalTimeMs = parallel ? *perLoop : *perLoop * node.loops.value_or(1.0);
            }

            static const std::vector<std::string> shown = {
                "Node Type",           "Join Type",    "Strategy",   "Parallel Aware",
                "Relation Name",       "Alias",        "CTE Name",   "Function Name",
                "Index Name",          "Plan Rows",    "Total Cost", "Actual Rows",
                "Actual Total Time",   "Actual Loops", "Plans",      "Startup Cost",
                "Actual Startup Time", "Async Capable"};
            for (const auto& [key, value] : j.items()) {
                if (std::ranges::find(shown, key) == shown.end()) {
                    node.properties.emplace_back(key, scalarText(value));
                }
            }
            if (const auto plans = j.find("Plans"); plans != j.end() && plans->is_array()) {
                for (const auto& child : *plans) {
                    node.children.push_back(postgresNode(
                        child, parallel || j.value("Node Type", "").starts_with("Gather")));
                }
            }
            return node;
        }

        std::optional<Plan> parsePostgres(const StatementResult& r, bool analyzed) {
            Plan plan;
            plan.raw = firstColumn(r);
            const json doc = json::parse(plan.raw, nullptr, false);
            const json* top = nullptr;
            if (doc.is_array() && !doc.empty() && doc[0].is_object()) {
                top = &doc[0];
            } else if (doc.is_object()) {
                top = &doc;
            }
            if (!top || !top->contains("Plan")) {
                return std::nullopt;
            }
            plan.root = postgresNode((*top)["Plan"]);
            plan.planningTimeMs = number(*top, "Planning Time");
            plan.executionTimeMs = number(*top, "Execution Time");
            plan.analyzed = analyzed;
            plan.raw = doc.dump(2);
            return plan;
        }

        // ---------------------------------------------------------------- mysql

        // -> Nested loop inner join  (cost=4.5 rows=5) (actual time=0.1..0.2 rows=5 loops=1)
        std::optional<Plan> parseMysqlTree(const StatementResult& r, bool analyzed) {
            static const std::regex costRe(
                R"(\(cost=([0-9.e+-]+)(?:\.\.([0-9.e+-]+))? rows=([0-9.e+-]+)\))");
            static const std::regex actualRe(
                R"(\(actual time=([0-9.e+-]+)\.\.([0-9.e+-]+) rows=([0-9.e+-]+) loops=([0-9.e+-]+)\))");
            static const std::regex neverRe(R"(\(never executed\))");

            Plan plan;
            plan.raw = firstColumn(r);
            plan.analyzed = analyzed;
            std::vector<std::pair<int, PlanNode>> flat; // depth, node
            std::istringstream lines(plan.raw);
            for (std::string line; std::getline(lines, line);) {
                const auto arrow = line.find("-> ");
                if (arrow == std::string::npos) {
                    // a wrapped line continues the node above it
                    if (!flat.empty() && line.find_first_not_of(' ') != std::string::npos) {
                        flat.back().second.operation +=
                            " " + line.substr(line.find_first_not_of(' '));
                    }
                    continue;
                }
                PlanNode node;
                std::string text = line.substr(arrow + 3);
                std::smatch m;
                if (std::regex_search(text, m, costRe)) {
                    node.cost = std::stod(m[2].matched ? m[2].str() : m[1].str());
                    node.estimatedRows = std::stod(m[3].str());
                }
                if (std::regex_search(text, m, actualRe)) {
                    node.actualRows = std::stod(m[3].str());
                    node.loops = std::stod(m[4].str());
                    // the time to the last row, averaged per loop
                    node.totalTimeMs = std::stod(m[2].str()) * *node.loops;
                } else if (std::regex_search(text, neverRe)) {
                    node.actualRows = 0;
                    node.loops = 0;
                    node.totalTimeMs = 0;
                }
                text = std::regex_replace(text, costRe, "");
                text = std::regex_replace(text, actualRe, "");
                text = std::regex_replace(text, neverRe, "");
                while (!text.empty() && text.back() == ' ') {
                    text.pop_back();
                }
                node.operation = text;
                flat.emplace_back(static_cast<int>(arrow), std::move(node));
            }
            if (flat.empty()) {
                return std::nullopt;
            }

            // indentation is the depth; build bottom-up so moves never invalidate a parent
            PlanNode root{.operation = "Query"};
            std::vector<std::pair<int, PlanNode*>> stack = {{-1, &root}};
            for (auto& [depth, node] : flat) {
                while (stack.back().first >= depth) {
                    stack.pop_back();
                }
                PlanNode* parent = stack.back().second;
                parent->children.push_back(std::move(node));
                stack.emplace_back(depth, &parent->children.back());
            }
            plan.root =
                root.children.size() == 1 ? std::move(root.children.front()) : std::move(root);
            return plan;
        }

        // ---------------------------------------------------------------- sqlite

        // EXPLAIN QUERY PLAN: id, parent, notused, detail
        std::optional<Plan> parseSqlite(const StatementResult& r) {
            if (r.columnNames.size() < 4) {
                return std::nullopt;
            }
            struct Row {
                long id;
                long parent;
                std::string detail;
            };
            std::vector<Row> rows;
            for (const auto& row : r.tableData) {
                if (row.size() < 4) {
                    continue;
                }
                try {
                    rows.push_back({std::stol(row[0]), std::stol(row[1]), row[3]});
                } catch (...) {
                    return std::nullopt;
                }
            }
            if (rows.empty()) {
                return std::nullopt;
            }

            Plan plan;
            // rows list parents before children: recurse on ids, no pointers into growing vectors
            auto build = [&](auto&& self, long parent, int depth) -> std::vector<PlanNode> {
                std::vector<PlanNode> out;
                for (const auto& row : rows) {
                    if (row.parent == parent && row.id != parent) {
                        plan.raw +=
                            std::string(static_cast<size_t>(depth) * 2, ' ') + row.detail + "\n";
                        PlanNode node{.operation = row.detail};
                        node.children = self(self, row.id, depth + 1);
                        out.push_back(std::move(node));
                    }
                }
                return out;
            };
            auto top = build(build, 0, 0);
            if (top.empty()) {
                return std::nullopt;
            }
            if (top.size() == 1) {
                plan.root = std::move(top.front());
            } else {
                plan.root = PlanNode{.operation = "QUERY PLAN", .children = std::move(top)};
            }
            return plan;
        }

    } // namespace

    std::optional<std::string> explainSql(DatabaseType type, std::string_view statement,
                                          bool analyze) {
        const std::string stmt = trimStatement(statement);
        if (stmt.empty()) {
            return std::nullopt;
        }
        switch (type) {
        case DatabaseType::POSTGRESQL:
            return (analyze ? "EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) "
                            : "EXPLAIN (FORMAT JSON) ") +
                   stmt;
        case DatabaseType::MYSQL:
            return (analyze ? "EXPLAIN ANALYZE " : "EXPLAIN FORMAT=TREE ") + stmt;
        case DatabaseType::SQLITE:
            if (analyze) {
                return std::nullopt;
            }
            return "EXPLAIN QUERY PLAN " + stmt;
        default:
            return std::nullopt;
        }
    }

    bool supportsExplainAnalyze(DatabaseType type) {
        return type == DatabaseType::POSTGRESQL || type == DatabaseType::MYSQL;
    }

    std::optional<Plan> parsePlan(DatabaseType type, const StatementResult& result, bool analyzed) {
        if (!result.success || result.tableData.empty()) {
            return std::nullopt;
        }
        std::optional<Plan> plan;
        switch (type) {
        case DatabaseType::POSTGRESQL:
            plan = parsePostgres(result, analyzed);
            break;
        case DatabaseType::MYSQL:
            plan = parseMysqlTree(result, analyzed);
            break;
        case DatabaseType::SQLITE:
            plan = parseSqlite(result);
            break;
        default:
            return std::nullopt;
        }
        if (plan) {
            fillSelfTimes(plan->root);
        }
        return plan;
    }

    // ---------- plan review ----------

    namespace {

        std::optional<std::string> property(const PlanNode& n, std::string_view key) {
            for (const auto& [k, v] : n.properties) {
                if (k == key) {
                    return v;
                }
            }
            return std::nullopt;
        }

        std::string rowCount(double v) {
            return std::format("{}", static_cast<long long>(v + 0.5));
        }

        // postgres "Seq Scan", mysql "Table scan on t", sqlite "SCAN t" (not "SCAN t USING
        // INDEX", which walks an index)
        bool isFullScan(const PlanNode& n) {
            const auto& op = n.operation;
            if (op.ends_with("Seq Scan") || op.starts_with("Table scan on ")) {
                return true;
            }
            return op.starts_with("SCAN ") && op.find(" USING ") == std::string::npos &&
                   op != "SCAN CONSTANT ROW" && op.size() > 5 && op[5] != '(';
        }

        std::string tableOf(const PlanNode& n) {
            const auto& op = n.operation;
            std::string name;
            if (op.starts_with("Table scan on ")) {
                name = op.substr(14);
            } else if (op.starts_with("SCAN ") || op.starts_with("SEARCH ")) {
                name = op.substr(op.find(' ') + 1);
            } else if (n.detail.starts_with("on ")) {
                name = n.detail.substr(3);
            }
            return name.substr(0, name.find(' '));
        }

        // rows the node returns per loop: actual when analyzed, else the estimate
        std::optional<double> rowsOut(const PlanNode& n, bool analyzed) {
            return analyzed && n.actualRows ? n.actualRows : n.estimatedRows;
        }

        void review(const PlanNode& n, const PlanNode* parent, size_t index, bool analyzed,
                    std::vector<PlanWarning>& out) {
            const std::string table = tableOf(n);
            const std::string what = table.empty() ? n.operation : table;

            if (isFullScan(n)) {
                // postgres: the filter sits on the scan; mysql: on a Filter node above it
                const auto pgFilter = property(n, "Filter");
                const bool mysqlFilter = parent && parent->operation.starts_with("Filter: ");
                std::optional<double> read;
                std::optional<double> kept;
                if (pgFilter && analyzed && n.actualRows) {
                    if (const auto removed = property(n, "Rows Removed by Filter")) {
                        try {
                            read = *n.actualRows + std::stod(*removed);
                            kept = n.actualRows;
                        } catch (...) {
                        }
                    }
                } else if (mysqlFilter) {
                    read = rowsOut(n, analyzed);
                    kept = rowsOut(*parent, analyzed);
                }
                if (read && kept && *read >= 1000 && *kept * 10 <= *read) {
                    out.push_back(
                        {&n, std::format("Full scan of {} reads {} rows to keep {}{}. An index on "
                                         "the filtered columns would read only the matches.",
                                         what, rowCount(*read), rowCount(*kept),
                                         pgFilter ? ", filter " + *pgFilter : "")});
                } else if (pgFilter && !analyzed && n.cost && *n.cost >= 1000) {
                    out.push_back(
                        {&n, std::format("Full scan of {} with filter {}, estimated cost {}. "
                                         "If the filter keeps few rows, an index on its columns "
                                         "would avoid reading the whole table.",
                                         what, *pgFilter, rowCount(*n.cost))});
                }

                // the inner side of a nested loop: read in full once per outer row
                bool inner = false;
                std::optional<double> outerRows;
                if (parent && index > 0) {
                    const auto& pop = parent->operation;
                    const auto& first = parent->children.front();
                    if (pop.find("Nested Loop") != std::string::npos ||
                        pop.find("Nested loop") != std::string::npos) {
                        inner = true;
                        outerRows = rowsOut(first, analyzed);
                    } else if (first.operation.starts_with("SCAN ") ||
                               first.operation.starts_with("SEARCH ")) {
                        inner = true; // sqlite: later scans of a join loop inside earlier ones
                    }
                }
                const auto times = analyzed && n.loops ? n.loops : outerRows;
                if (inner && (!times || *times >= 10)) {
                    out.push_back(
                        {&n, std::format("{} is read in full once per row of the outer "
                                         "side{}. An index on its join columns would "
                                         "turn each pass into a lookup.",
                                         what, times ? " (" + rowCount(*times) + " times)" : "")});
                }
            }

            const auto& op = n.operation;
            if (op.find("AUTOMATIC") != std::string::npos &&
                op.find("INDEX") != std::string::npos) {
                out.push_back({&n, std::format("SQLite builds a temporary index on {} every time "
                                               "this runs. CREATE INDEX it once instead.",
                                               what)});
            }
            if (const auto method = property(n, "Sort Method");
                method && method->find("external") != std::string::npos) {
                const auto space = property(n, "Sort Space Used");
                out.push_back({&n, std::format("The sort spilled to disk{}. More work_mem, or an "
                                               "index matching the ORDER BY, would avoid it.",
                                               space ? " (" + *space + " kB)" : "")});
            } else if ((op == "Sort" || op.starts_with("Sort: ")) &&
                       rowsOut(n, analyzed).value_or(0) >= 100000) {
                out.push_back({&n, std::format("Sorts {} rows. An index matching the ORDER BY "
                                               "would return them in order.",
                                               rowCount(*rowsOut(n, analyzed)))});
            }

            if (analyzed && n.estimatedRows && n.actualRows && n.loops.value_or(1) > 0) {
                const double est = std::max(*n.estimatedRows, 1.0);
                const double act = std::max(*n.actualRows, 1.0);
                if ((est / act >= 10 || act / est >= 10) && std::max(est, act) >= 1000) {
                    out.push_back({&n, std::format("Expected {} rows, got {}: the table "
                                                   "statistics may be stale (run ANALYZE), and "
                                                   "the plan may be built on the wrong guess.",
                                                   rowCount(est), rowCount(act))});
                }
            }

            for (size_t i = 0; i < n.children.size(); ++i) {
                review(n.children[i], &n, i, analyzed, out);
            }
        }

    } // namespace

    std::vector<PlanWarning> reviewPlan(const Plan& plan) {
        std::vector<PlanWarning> out;
        review(plan.root, nullptr, 0, plan.analyzed, out);
        return out;
    }

} // namespace dearsql
