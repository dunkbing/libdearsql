#pragma once

#include "dearsql/dearsql.hpp"
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
// "skip if backend not implemented yet" check, so integration tests light up
// automatically once a backend skeleton is fleshed out.
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
            GTEST_SKIP() << "backend skeleton: " << (status).second;                               \
        }                                                                                          \
    } while (0)

#define DEARSQL_SKIP_IF_NO_CONFIG(opt, key)                                                        \
    do {                                                                                           \
        if (!(opt))                                                                                \
            GTEST_SKIP() << "no test config: set " << (key)                                        \
                         << " (run scripts/test-remote to spin up containers)";                    \
    } while (0)

} // namespace dearsql::testing
