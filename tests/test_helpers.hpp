#pragma once

#include "dearsql/dearsql.hpp"
#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <thread>

namespace dearsql::testing {

struct BackendConfig {
    std::string name;
    std::string host;
    int port = 0;
    std::string database;
    std::string username;
    std::string password;
    std::string path; // SQLite only
};

inline const char* envOr(const char* key, const char* fallback = nullptr) {
    if (!key) {
        return fallback;
    }
    const char* v = std::getenv(key);
    return (v && *v) ? v : fallback;
}

inline std::optional<BackendConfig> loadConfig(const char* host_key, const char* port_key,
                                               const char* db_key, const char* user_key,
                                               const char* pass_key, const char* name_key,
                                               int default_port) {
    const char* h = envOr(host_key);
    if (!h)
        return std::nullopt;
    BackendConfig c;
    c.host = h;
    if (const char* p = envOr(port_key))
        c.port = std::stoi(p);
    else
        c.port = default_port;
    if (const char* d = envOr(db_key))
        c.database = d;
    if (const char* u = envOr(user_key))
        c.username = u;
    if (const char* pw = envOr(pass_key))
        c.password = pw;
    if (const char* n = envOr(name_key))
        c.name = n;
    return c;
}

// Wraps the cfg→ConnectionInfo→makeConnection→open dance with a uniform
// "skip if backend not configured" shape for optional integration tests.
struct OpenResult {
    ConnectionPtr conn;
    Status status;
};

inline OpenResult tryOpen(const BackendConfig& cfg, DatabaseType type) {
    ConnectionInfo info;
    info.type = type;
    info.name = cfg.name;
    info.host = cfg.host;
    info.port = cfg.port;
    info.database = cfg.database;
    info.username = cfg.username;
    info.password = cfg.password;
    info.path = cfg.path;
    info.sslmode = SslMode::Disable;

    auto conn = makeConnection(info);
    if (!conn)
        return {nullptr, {false, "makeConnection returned nullptr"}};

    Status s{false, ""};
    for (int attempt = 0; attempt < 30; ++attempt) {
        s = conn->open();
        if (s.first)
            return {conn, s};
        if (s.second.find("not implemented") != std::string::npos)
            return {conn, s};
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return {conn, s};
}

#define DEARSQL_SKIP_IF_UNIMPLEMENTED(status)                                                      \
    do {                                                                                           \
        if (!(status).first && (status).second.find("not implemented") != std::string::npos) {    \
            GTEST_SKIP() << "backend unavailable: " << (status).second;                            \
        }                                                                                          \
    } while (0)

#define DEARSQL_SKIP_IF_NO_CONFIG(opt, key)                                                        \
    do {                                                                                           \
        if (!(opt))                                                                                \
            GTEST_SKIP() << "no test config: set " << (key)                                        \
                         << " (run scripts/test-remote to spin up containers)";                    \
    } while (0)


// deleteEffects against a live server: the counts its queries give must be what the delete
// then does. `db` runs the statements and lists the catalog (a schema where there are any);
// plain ddl that every engine takes (NULL spelled out: sql server sessions default to NOT NULL), tables named <prefix>_*, dropped at the end
inline void checkDeleteImpact(IDatabase& db, DatabaseType type, const std::string& p) {
    auto run = [&](const std::string& sql) {
        const auto r = db.execute(sql, 0);
        EXPECT_TRUE(r.success()) << sql << ": " << (r.empty() ? "" : r[0].errorMessage);
    };
    auto count = [&](const std::string& sql) {
        const auto r = db.execute(sql, 0);
        EXPECT_TRUE(r.success()) << sql << ": " << (r.empty() ? "" : r[0].errorMessage);
        return r.success() && !r.empty() && !r[0].tableData.empty()
                   ? std::stoi(r[0].tableData[0][0])
                   : -1;
    };
    const std::vector<std::string> drops = {p + "_invoices", p + "_notes", p + "_items",
                                            p + "_orders", p + "_customers"};
    for (const auto& t : drops)
        db.execute("DROP TABLE " + t, 0); // leftovers from a failed run
    run("CREATE TABLE " + p + "_customers (id INT PRIMARY KEY)");
    run("CREATE TABLE " + p + "_orders (id INT PRIMARY KEY, customer_id INT NULL, region VARCHAR(8), " +
        "UNIQUE (id, region), FOREIGN KEY (customer_id) REFERENCES " + p +
        "_customers (id) ON DELETE CASCADE)");
    run("CREATE TABLE " + p + "_items (id INT PRIMARY KEY, order_id INT, region VARCHAR(8), " +
        "FOREIGN KEY (order_id, region) REFERENCES " + p +
        "_orders (id, region) ON DELETE CASCADE)");
    run("CREATE TABLE " + p + "_notes (id INT PRIMARY KEY, customer_id INT NULL, FOREIGN KEY " +
        "(customer_id) REFERENCES " + p + "_customers (id) ON DELETE SET NULL)");
    run("CREATE TABLE " + p + "_invoices (id INT PRIMARY KEY, customer_id INT NULL, FOREIGN KEY " +
        "(customer_id) REFERENCES " + p + "_customers (id))");
    for (const auto& sql : {
             "INSERT INTO " + p + "_customers (id) VALUES (1)",
             "INSERT INTO " + p + "_customers (id) VALUES (2)",
             "INSERT INTO " + p + "_orders (id, customer_id, region) VALUES (10, 1, 'eu')",
             "INSERT INTO " + p + "_orders (id, customer_id, region) VALUES (11, 2, 'us')",
             "INSERT INTO " + p + "_items (id, order_id, region) VALUES (100, 10, 'eu')",
             "INSERT INTO " + p + "_items (id, order_id, region) VALUES (101, 10, 'eu')",
             "INSERT INTO " + p + "_items (id, order_id, region) VALUES (102, 11, 'us')",
             "INSERT INTO " + p + "_notes (id, customer_id) VALUES (1, 1)",
             "INSERT INTO " + p + "_invoices (id, customer_id) VALUES (1, 2)",
         })
        run(sql);

    auto lower = [](std::string s) {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    const auto tables = db.tables();
    const auto root = std::ranges::find_if(
        tables, [&](const Table& t) { return lower(t.name) == p + "_customers"; });
    ASSERT_NE(root, tables.end());
    const auto effects = deleteEffects(type, tables, *root, "id = 1");
    auto effect = [&](const std::string& path) -> const DeleteEffect* {
        for (const auto& e : effects)
            if (lower(e.path) == path)
                return &e;
        ADD_FAILURE() << "no effect " << path;
        return nullptr;
    };
    const std::string c = p + "_customers > ";
    const auto* orders = effect(c + p + "_orders");
    const auto* items = effect(c + p + "_orders > " + p + "_items");
    const auto* notes = effect(c + p + "_notes");
    const auto* invoices = effect(c + p + "_invoices");
    if (orders && items && notes && invoices) {
        EXPECT_EQ(orders->action, "CASCADE");
        EXPECT_EQ(items->action, "CASCADE");
        EXPECT_EQ(notes->action, "SET NULL");
        EXPECT_TRUE(invoices->action == "NO ACTION" || invoices->action == "RESTRICT")
            << invoices->action;
        EXPECT_EQ(count(orders->countSql), 1);
        EXPECT_EQ(count(items->countSql), 2);
        EXPECT_EQ(count(notes->countSql), 1);
        EXPECT_EQ(count(invoices->countSql), 0);
    }
    run("DELETE FROM " + p + "_customers WHERE id = 1");
    EXPECT_EQ(count("SELECT COUNT(*) FROM " + p + "_orders"), 1);
    EXPECT_EQ(count("SELECT COUNT(*) FROM " + p + "_items"), 1);
    EXPECT_EQ(count("SELECT COUNT(*) FROM " + p + "_notes WHERE customer_id IS NULL"), 1);
    for (const auto& t : drops)
        run("DROP TABLE " + t);
}

} // namespace dearsql::testing
