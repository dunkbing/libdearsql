#include <dearsql/completion.hpp>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

using namespace dearsql;

namespace {

Table makeTable(std::string schema, std::string name,
                std::vector<std::pair<std::string, std::string>> cols) {
    Table t;
    t.schema = std::move(schema);
    t.name = std::move(name);
    for (auto& [n, type] : cols) {
        Column c;
        c.name = n;
        c.type = type;
        t.columns.push_back(std::move(c));
    }
    return t;
}

CompletionCatalog pgCatalog() {
    CompletionCatalog cat;
    cat.defaultSchema = "public";
    cat.tables.push_back(
        makeTable("public", "users", {{"id", "integer"}, {"name", "text"}, {"email", "text"}}));
    cat.tables.push_back(makeTable("public", "orders",
                                   {{"id", "integer"},
                                    {"user_id", "integer"},
                                    {"total", "numeric"},
                                    {"created_at", "timestamp"},
                                    {"order", "text"}}));
    cat.tables.push_back(makeTable("public", "Order Items", {{"qty", "integer"}}));
    cat.tables.push_back(makeTable("sales", "invoices", {{"amount", "numeric"}}));
    cat.views.push_back(makeTable("public", "active_users", {{"id", "integer"}}));
    cat.sequences.push_back({"public", "orders_id_seq"});
    return cat;
}

struct Run {
    CompletionResult result;
    std::string sql;
    size_t cursor = 0;
};

// '|' marks the cursor
Run run(std::string text, const CompletionCatalog& cat,
        DatabaseType type = DatabaseType::POSTGRESQL) {
    const size_t pos = text.find('|');
    text.erase(pos, 1);
    Run r;
    r.result = complete(text, pos, cat, type);
    r.sql = std::move(text);
    r.cursor = pos;
    return r;
}

const CompletionItem* find(const CompletionResult& r, std::string_view label, CompletionKind kind) {
    for (const auto& it : r.items) {
        if (it.label == label && it.kind == kind)
            return &it;
    }
    return nullptr;
}

int rankOf(const CompletionResult& r, std::string_view label) {
    for (size_t i = 0; i < r.items.size(); ++i) {
        if (r.items[i].label == label)
            return static_cast<int>(i);
    }
    return -1;
}

bool hasKind(const CompletionResult& r, CompletionKind kind) {
    for (const auto& it : r.items) {
        if (it.kind == kind)
            return true;
    }
    return false;
}

std::pair<size_t, size_t> at(const std::string& sql, std::string_view needle) {
    const size_t p = sql.find(needle);
    return {p, p + needle.size()};
}

} // namespace

TEST(Completion, KeywordAtStatementStart) {
    const auto cat = pgCatalog();
    auto r = run("SEL|", cat);
    ASSERT_FALSE(r.result.items.empty());
    EXPECT_EQ(r.result.items[0].label, "SELECT");
    EXPECT_EQ(r.result.items[0].kind, CompletionKind::Keyword);
    EXPECT_EQ(r.result.items[0].insertText, "SELECT");
    EXPECT_EQ(r.result.replaceStart, 0u);
    EXPECT_EQ(r.result.replaceEnd, 3u);
    EXPECT_FALSE(hasKind(r.result, CompletionKind::Table));

    // keywords follow the typed case
    auto lower = run("sel|", cat);
    ASSERT_FALSE(lower.result.items.empty());
    EXPECT_EQ(lower.result.items[0].insertText, "select");
}

TEST(Completion, FromOffersRelations) {
    const auto cat = pgCatalog();
    auto r = run("SELECT * FROM us|", cat);
    ASSERT_NE(find(r.result, "users", CompletionKind::Table), nullptr);
    EXPECT_EQ(rankOf(r.result, "users"), 0);
    EXPECT_NE(find(r.result, "active_users", CompletionKind::View), nullptr);
    EXPECT_FALSE(hasKind(r.result, CompletionKind::Column));
    EXPECT_FALSE(hasKind(r.result, CompletionKind::Function));

    // objects outside the default schema insert qualified
    auto inv = run("SELECT * FROM inv|", cat);
    const auto* item = find(inv.result, "invoices", CompletionKind::Table);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->insertText, "sales.invoices");

    // JOIN and comma lists are relation slots too
    EXPECT_NE(
        find(run("SELECT * FROM users u JOIN or|", cat).result, "orders", CompletionKind::Table),
        nullptr);
    EXPECT_NE(find(run("SELECT * FROM users, or|", cat).result, "orders", CompletionKind::Table),
              nullptr);
}

TEST(Completion, SchemaMembers) {
    const auto cat = pgCatalog();
    auto r = run("SELECT * FROM sales.|", cat);
    ASSERT_EQ(r.result.items.size(), 1u);
    EXPECT_EQ(r.result.items[0].label, "invoices");
    EXPECT_EQ(r.result.items[0].insertText, "invoices");
    EXPECT_EQ(r.result.replaceStart, r.cursor);
    EXPECT_EQ(r.result.replaceEnd, r.cursor);
}

TEST(Completion, AfterRelationOffersClauseKeywords) {
    const auto cat = pgCatalog();
    auto r = run("SELECT * FROM users wh|", cat);
    ASSERT_FALSE(r.result.items.empty());
    EXPECT_EQ(r.result.items[0].label, "WHERE");
    EXPECT_FALSE(hasKind(r.result, CompletionKind::Table));
}

TEST(Completion, AliasColumns) {
    const auto cat = pgCatalog();
    auto r = run("SELECT u.| FROM users u", cat);
    EXPECT_NE(find(r.result, "email", CompletionKind::Column), nullptr);
    EXPECT_NE(find(r.result, "name", CompletionKind::Column), nullptr);
    EXPECT_EQ(find(r.result, "total", CompletionKind::Column), nullptr);
    EXPECT_FALSE(hasKind(r.result, CompletionKind::Keyword));
    const auto* email = find(r.result, "email", CompletionKind::Column);
    ASSERT_NE(email, nullptr);
    EXPECT_EQ(email->detail, "text");

    auto join = run("SELECT * FROM users AS u JOIN orders o ON o.us| = u.id", cat);
    ASSERT_FALSE(join.result.items.empty());
    EXPECT_EQ(join.result.items[0].label, "user_id");
    EXPECT_EQ(find(join.result, "email", CompletionKind::Column), nullptr);

    // a table name works as its own qualifier
    auto bare = run("SELECT users.| FROM users", cat);
    EXPECT_NE(find(bare.result, "email", CompletionKind::Column), nullptr);
}

TEST(Completion, ColumnsInScopeComeFirst) {
    const auto cat = pgCatalog();
    auto r = run("SELECT * FROM orders WHERE to|", cat);
    ASSERT_FALSE(r.result.items.empty());
    EXPECT_EQ(r.result.items[0].label, "total");
    EXPECT_EQ(r.result.items[0].kind, CompletionKind::Column);
    // columns of tables not in the statement are not offered
    EXPECT_EQ(
        find(run("SELECT * FROM orders WHERE em|", cat).result, "email", CompletionKind::Column),
        nullptr);

    // select list before FROM sees tables after the cursor
    auto sel = run("SELECT na| FROM users", cat);
    ASSERT_FALSE(sel.result.items.empty());
    EXPECT_EQ(sel.result.items[0].label, "name");

    // functions after columns, in the typed case
    auto fn = run("SELECT cou| FROM users", cat);
    const auto* count = find(fn.result, "COUNT", CompletionKind::Function);
    ASSERT_NE(count, nullptr);
    EXPECT_EQ(count->insertText, "count");
}

TEST(Completion, CteColumnsAndName) {
    const auto cat = pgCatalog();
    auto r = run("WITH recent AS (SELECT id, total AS amount FROM orders) "
                 "SELECT r.| FROM recent r",
                 cat);
    EXPECT_NE(find(r.result, "id", CompletionKind::Column), nullptr);
    EXPECT_NE(find(r.result, "amount", CompletionKind::Column), nullptr);
    EXPECT_EQ(find(r.result, "total", CompletionKind::Column), nullptr);
    // the type follows the source column
    const auto* id = find(r.result, "id", CompletionKind::Column);
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->detail, "integer");

    auto from = run("WITH recent AS (SELECT id FROM orders) SELECT * FROM rec|", cat);
    const auto* cte = find(from.result, "recent", CompletionKind::Table);
    ASSERT_NE(cte, nullptr);
    EXPECT_EQ(cte->detail, "cte");

    auto list = run("WITH x(a, b) AS (SELECT 1, 2) SELECT x.| FROM x", cat);
    EXPECT_NE(find(list.result, "a", CompletionKind::Column), nullptr);
    EXPECT_NE(find(list.result, "b", CompletionKind::Column), nullptr);
}

TEST(Completion, SubqueryAlias) {
    const auto cat = pgCatalog();
    auto r = run("SELECT s.| FROM (SELECT name, email AS mail FROM users) s", cat);
    EXPECT_NE(find(r.result, "name", CompletionKind::Column), nullptr);
    EXPECT_NE(find(r.result, "mail", CompletionKind::Column), nullptr);
    EXPECT_EQ(find(r.result, "email", CompletionKind::Column), nullptr);

    auto star = run("SELECT s.| FROM (SELECT * FROM users) AS s", cat);
    EXPECT_NE(find(star.result, "email", CompletionKind::Column), nullptr);
    EXPECT_NE(find(star.result, "id", CompletionKind::Column), nullptr);

    // a correlated subquery sees the outer alias
    auto corr = run("SELECT * FROM users u WHERE EXISTS "
                    "(SELECT 1 FROM orders o WHERE o.user_id = u.|)",
                    cat);
    EXPECT_NE(find(corr.result, "email", CompletionKind::Column), nullptr);

    // a scalar subquery's tables stay inside it
    auto scalar = run("SELECT (SELECT max(total) FROM orders), na| FROM users", cat);
    EXPECT_NE(find(scalar.result, "name", CompletionKind::Column), nullptr);
    EXPECT_EQ(find(run("SELECT (SELECT 1 FROM orders), to| FROM users", cat).result, "total",
                   CompletionKind::Column),
              nullptr);
}

TEST(Completion, NothingInsideStringsAndComments) {
    const auto cat = pgCatalog();
    EXPECT_TRUE(run("SELECT 'us|' FROM users", cat).result.items.empty());
    EXPECT_TRUE(run("SELECT 'it''s us|", cat).result.items.empty());
    EXPECT_TRUE(run("SELECT * FROM users -- FROM us|", cat).result.items.empty());
    EXPECT_TRUE(run("SELECT * /* FROM us| */ FROM users", cat).result.items.empty());
    EXPECT_TRUE(run("CREATE FUNCTION f() RETURNS int AS $$ SELECT us| $$ LANGUAGE sql", cat)
                    .result.items.empty());
    EXPECT_TRUE(run("SELECT $tag$ us| $tag$", cat).result.items.empty());
    EXPECT_TRUE(run("SELECT * FROM users LIMIT 10|", cat).result.items.empty());
    // mysql: double quotes are strings, # starts a comment
    EXPECT_TRUE(run("SELECT \"us|\" FROM users", cat, DatabaseType::MYSQL).result.items.empty());
    EXPECT_TRUE(run("SELECT 1 # us|", cat, DatabaseType::MYSQL).result.items.empty());

    // after a closed comment completion works again
    auto after = run("SELECT * /* c */ FROM us|", cat);
    EXPECT_NE(find(after.result, "users", CompletionKind::Table), nullptr);
    // a ';' inside a string does not split the statement
    auto semi = run("SELECT ';' AS x, u.| FROM users u", cat);
    EXPECT_NE(find(semi.result, "email", CompletionKind::Column), nullptr);
}

TEST(Completion, OnlyTheCurrentStatement) {
    const auto cat = pgCatalog();
    auto r = run("SELECT * FROM orders o; SELECT u.| FROM users u", cat);
    EXPECT_NE(find(r.result, "email", CompletionKind::Column), nullptr);

    // an alias from the previous statement is not in scope
    auto prev = run("SELECT * FROM users u; SELECT u.| FROM orders", cat);
    EXPECT_EQ(find(prev.result, "email", CompletionKind::Column), nullptr);

    // a new statement after ';' starts over
    auto start = run("SELECT 1; SEL|", cat);
    ASSERT_FALSE(start.result.items.empty());
    EXPECT_EQ(start.result.items[0].label, "SELECT");

    const std::string sql = "SELECT 1;\n  SELECT 2 ; SELECT 3";
    EXPECT_EQ(statementRangeAt(sql, 14, DatabaseType::POSTGRESQL), at(sql, "SELECT 2"));
    EXPECT_EQ(statementRangeAt(sql, 3, DatabaseType::POSTGRESQL), at(sql, "SELECT 1"));
    EXPECT_EQ(statementRangeAt(sql, sql.size(), DatabaseType::POSTGRESQL), at(sql, "SELECT 3"));
}

TEST(Completion, QuotesOddNames) {
    const auto cat = pgCatalog();
    auto r = run("SELECT * FROM Ord|", cat);
    const auto* items = find(r.result, "Order Items", CompletionKind::Table);
    ASSERT_NE(items, nullptr);
    EXPECT_EQ(items->insertText, "\"Order Items\"");

    // reserved word as a column
    auto col = run("SELECT ord| FROM orders", cat);
    const auto* order = find(col.result, "order", CompletionKind::Column);
    ASSERT_NE(order, nullptr);
    EXPECT_EQ(order->insertText, "\"order\"");

    // per-dialect quotes
    auto my = run("SELECT * FROM Ord|", cat, DatabaseType::MYSQL);
    ASSERT_NE(find(my.result, "Order Items", CompletionKind::Table), nullptr);
    EXPECT_EQ(find(my.result, "Order Items", CompletionKind::Table)->insertText, "`Order Items`");
    auto ms = run("SELECT * FROM Ord|", cat, DatabaseType::MSSQL);
    ASSERT_NE(find(ms.result, "Order Items", CompletionKind::Table), nullptr);
    EXPECT_EQ(find(ms.result, "Order Items", CompletionKind::Table)->insertText, "[Order Items]");

    // case folding
    EXPECT_TRUE(identifierNeedsQuoting("UserLog", DatabaseType::POSTGRESQL));
    EXPECT_FALSE(identifierNeedsQuoting("UserLog", DatabaseType::MYSQL));
    EXPECT_FALSE(identifierNeedsQuoting("USERS", DatabaseType::ORACLE));
    EXPECT_TRUE(identifierNeedsQuoting("users", DatabaseType::ORACLE));
    EXPECT_FALSE(identifierNeedsQuoting("user_id", DatabaseType::POSTGRESQL));
    EXPECT_TRUE(identifierNeedsQuoting("select", DatabaseType::SQLITE));
    EXPECT_EQ(quoteIdentifierIfNeeded("a\"b", DatabaseType::POSTGRESQL), "\"a\"\"b\"");
}

TEST(Completion, DialectKeywords) {
    const auto cat = pgCatalog();
    EXPECT_NE(find(run("SELECT * FROM users WHERE name IL|", cat).result, "ILIKE",
                   CompletionKind::Keyword),
              nullptr);
    EXPECT_EQ(find(run("SELECT * FROM users WHERE name IL|", cat, DatabaseType::MYSQL).result,
                   "ILIKE", CompletionKind::Keyword),
              nullptr);
    EXPECT_NE(
        find(run("SELECT TO|", cat, DatabaseType::MSSQL).result, "TOP", CompletionKind::Keyword),
        nullptr);

    auto has = [](const std::vector<std::string>& v, std::string_view w) {
        return std::find(v.begin(), v.end(), w) != v.end();
    };
    EXPECT_TRUE(has(sqlKeywords(DatabaseType::POSTGRESQL), "ILIKE"));
    EXPECT_FALSE(has(sqlKeywords(DatabaseType::SQLITE), "ILIKE"));
    EXPECT_TRUE(has(sqlKeywords(DatabaseType::SQLITE), "PRAGMA"));
    EXPECT_TRUE(has(sqlKeywords(DatabaseType::CASSANDRA), "FILTERING"));
    EXPECT_TRUE(has(sqlFunctions(DatabaseType::MYSQL), "GROUP_CONCAT"));
    EXPECT_FALSE(has(sqlFunctions(DatabaseType::POSTGRESQL), "GROUP_CONCAT"));
    EXPECT_TRUE(sqlKeywords(DatabaseType::REDIS).empty());
    EXPECT_TRUE(run("SEL|", cat, DatabaseType::MONGODB).result.items.empty());
}

TEST(Completion, ReplaceRange) {
    const auto cat = pgCatalog();
    // the whole word under the cursor is replaced, filtered by the part before it
    auto mid = run("SELECT * FROM us|ers", cat);
    EXPECT_EQ(mid.result.replaceStart, 14u);
    EXPECT_EQ(mid.result.replaceEnd, 19u);
    EXPECT_NE(find(mid.result, "users", CompletionKind::Table), nullptr);

    // a word equal to an item offers nothing more for it
    EXPECT_EQ(find(run("SELECT * FROM users|", cat).result, "users", CompletionKind::Table),
              nullptr);

    // inside a quoted identifier the quotes are part of the range
    auto quoted = run("SELECT * FROM \"Ord|\"", cat);
    EXPECT_EQ(quoted.result.replaceStart, 14u);
    EXPECT_EQ(quoted.result.replaceEnd, quoted.sql.size());
    const auto* item = find(quoted.result, "Order Items", CompletionKind::Table);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->insertText, "\"Order Items\"");
    EXPECT_FALSE(hasKind(quoted.result, CompletionKind::Keyword));

    // empty range at the cursor when no word is under it
    auto empty = run("SELECT * FROM |", cat);
    EXPECT_EQ(empty.result.replaceStart, empty.cursor);
    EXPECT_EQ(empty.result.replaceEnd, empty.cursor);
}

TEST(Completion, TargetColumns) {
    const auto cat = pgCatalog();
    auto r = run("INSERT INTO orders (|", cat);
    EXPECT_NE(find(r.result, "total", CompletionKind::Column), nullptr);
    EXPECT_EQ(find(r.result, "email", CompletionKind::Column), nullptr);
    EXPECT_FALSE(hasKind(r.result, CompletionKind::Keyword));

    auto set = run("UPDATE users SET em|", cat);
    ASSERT_FALSE(set.result.items.empty());
    EXPECT_EQ(set.result.items[0].label, "email");

    EXPECT_TRUE(run("SELECT total AS |", cat).result.items.empty());
}

TEST(Completion, TwoLevelSchemas) {
    CompletionCatalog cat;
    cat.defaultSchema = "shop.dbo";
    cat.tables.push_back(makeTable("shop.dbo", "customers", {{"id", "int"}}));
    cat.tables.push_back(makeTable("shop.sales", "orders", {{"id", "int"}}));
    cat.tables.push_back(makeTable("audit.dbo", "logs", {{"id", "int"}}));

    auto local = run("SELECT * FROM cus|", cat, DatabaseType::MSSQL);
    ASSERT_NE(find(local.result, "customers", CompletionKind::Table), nullptr);
    EXPECT_EQ(find(local.result, "customers", CompletionKind::Table)->insertText, "customers");

    auto sameDb = run("SELECT * FROM ord|", cat, DatabaseType::MSSQL);
    ASSERT_NE(find(sameDb.result, "orders", CompletionKind::Table), nullptr);
    EXPECT_EQ(find(sameDb.result, "orders", CompletionKind::Table)->insertText, "sales.orders");

    auto otherDb = run("SELECT * FROM lo|", cat, DatabaseType::MSSQL);
    ASSERT_NE(find(otherDb.result, "logs", CompletionKind::Table), nullptr);
    EXPECT_EQ(find(otherDb.result, "logs", CompletionKind::Table)->insertText, "audit.dbo.logs");

    auto dbo = run("SELECT * FROM dbo.|", cat, DatabaseType::MSSQL);
    EXPECT_NE(find(dbo.result, "customers", CompletionKind::Table), nullptr);
    EXPECT_NE(find(dbo.result, "logs", CompletionKind::Table), nullptr);
    EXPECT_EQ(find(dbo.result, "orders", CompletionKind::Table), nullptr);

    auto db = run("SELECT * FROM audit.|", cat, DatabaseType::MSSQL);
    EXPECT_NE(find(db.result, "dbo", CompletionKind::Schema), nullptr);
}

TEST(Completion, ResolveIdentifier) {
    const auto cat = pgCatalog();
    const std::string sql = "SELECT u.email, total FROM users u JOIN orders o ON o.user_id = u.id";

    auto col = resolveIdentifierAt(sql, sql.find("email") + 2, cat, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(col.has_value());
    EXPECT_EQ(col->kind, CompletionKind::Column);
    EXPECT_EQ(col->column, "email");
    EXPECT_EQ(col->relation, "users");
    EXPECT_EQ(col->alias, "u");
    ASSERT_NE(col->columnInfo, nullptr);
    EXPECT_EQ(col->columnInfo->type, "text");
    EXPECT_EQ(col->start, sql.find("email"));
    EXPECT_EQ(col->end, sql.find("email") + 5);

    auto bare = resolveIdentifierAt(sql, sql.find("total"), cat, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(bare->kind, CompletionKind::Column);
    EXPECT_EQ(bare->relation, "orders");

    auto alias = resolveIdentifierAt(sql, sql.find("u.email"), cat, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(alias.has_value());
    EXPECT_EQ(alias->kind, CompletionKind::Table);
    EXPECT_EQ(alias->relation, "users");
    ASSERT_NE(alias->table, nullptr);
    EXPECT_EQ(alias->table->name, "users");

    auto table = resolveIdentifierAt(sql, sql.find("orders") + 1, cat, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(table.has_value());
    EXPECT_EQ(table->kind, CompletionKind::Table);
    EXPECT_EQ(table->relation, "orders");

    const std::string cte = "WITH r AS (SELECT total AS amount FROM orders) SELECT amount FROM r";
    auto viaCte = resolveIdentifierAt(cte, cte.rfind("amount"), cat, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(viaCte.has_value());
    EXPECT_EQ(viaCte->kind, CompletionKind::Column);
    EXPECT_EQ(viaCte->relation, "r");

    const std::string schema = "SELECT * FROM sales.invoices";
    auto sch = resolveIdentifierAt(schema, schema.find("sales"), cat, DatabaseType::POSTGRESQL);
    ASSERT_TRUE(sch.has_value());
    EXPECT_EQ(sch->kind, CompletionKind::Schema);

    EXPECT_FALSE(resolveIdentifierAt(sql, 2, cat, DatabaseType::POSTGRESQL).has_value());
}

TEST(Completion, QuotedRelationsAndEmptyPrefix) {
    const auto cat = pgCatalog();
    auto pg = run("SELECT oi.| FROM \"Order Items\" oi", cat);
    EXPECT_NE(find(pg.result, "qty", CompletionKind::Column), nullptr);

    auto my = run("SELECT oi.| FROM `Order Items` AS oi", cat, DatabaseType::MYSQL);
    EXPECT_NE(find(my.result, "qty", CompletionKind::Column), nullptr);

    auto ms = run("SELECT oi.| FROM [Order Items] oi", cat, DatabaseType::MSSQL);
    EXPECT_NE(find(ms.result, "qty", CompletionKind::Column), nullptr);

    // with nothing typed, columns in scope lead, then the alias, then the rest
    auto empty = run("SELECT * FROM users u WHERE |", cat);
    ASSERT_GE(empty.result.items.size(), 4u);
    for (size_t i = 0; i < 3; ++i)
        EXPECT_EQ(empty.result.items[i].kind, CompletionKind::Column);
    EXPECT_EQ(empty.result.items[3].kind, CompletionKind::Alias);
    EXPECT_EQ(empty.result.items[3].label, "u");
    EXPECT_EQ(empty.result.items[3].detail, "users");
}

// "from al|" offers the table, not ALTER/DECIMAL/FALSE
TEST(Completion, FromPrefixSkipsUnrelatedKeywords) {
    CompletionCatalog cat;
    cat.tables.push_back(makeTable("", "albums", {{"id", "INTEGER"}}));
    auto r = run("select * from al|", cat, DatabaseType::SQLITE);
    ASSERT_FALSE(r.result.items.empty());
    EXPECT_EQ(r.result.items.front().label, "albums");
    for (const auto* kw : {"ALL", "ALTER", "DECIMAL", "FALSE", "NATURAL"})
        EXPECT_EQ(find(r.result, kw, CompletionKind::Keyword), nullptr) << kw;
    EXPECT_EQ(find(run("select fals|", cat, DatabaseType::SQLITE).result, "FALSE",
                   CompletionKind::Keyword) != nullptr,
              true);
}
