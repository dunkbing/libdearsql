#ifdef _WIN32
#include <winsock2.h>
#endif

#include "dearsql/backends/redis_connection.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <hiredis/hiredis.h>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#if __has_include(<hiredis/hiredis_ssl.h>)
#include <hiredis/hiredis_ssl.h>
#define DEARSQL_HAS_HIREDIS_SSL 1
#else
#define DEARSQL_HAS_HIREDIS_SSL 0
#endif

namespace dearsql {

namespace {

#if DEARSQL_HAS_HIREDIS_SSL
struct RedisSSLInit {
    RedisSSLInit() {
        redisInitOpenSSL();
    }
};
RedisSSLInit redisSSLInitOnce;
#endif

struct RedisReplyDeleter {
    void operator()(redisReply* r) const {
        if (r)
            freeReplyObject(r);
    }
};
using RedisReplyPtr = std::unique_ptr<redisReply, RedisReplyDeleter>;

RedisReplyPtr wrapReply(void* r) {
    return RedisReplyPtr(static_cast<redisReply*>(r));
}

std::vector<std::string> parseRedisCommand(const std::string& command) {
    std::vector<std::string> parts;
    std::string current;
    bool inQuotes = false;
    char quoteChar = '\0';

    for (char c : command) {
        if (!inQuotes) {
            if (c == '"' || c == '\'') {
                inQuotes = true;
                quoteChar = c;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current += c;
            }
        } else {
            if (c == quoteChar) {
                inQuotes = false;
                quoteChar = '\0';
            } else {
                current += c;
            }
        }
    }
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

std::string formatRedisReply(redisReply* reply) {
    if (!reply)
        return std::string(NULL_SENTINEL);

    switch (reply->type) {
    case REDIS_REPLY_STRING:
        return {reply->str, reply->len};
    case REDIS_REPLY_ARRAY: {
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i < reply->elements; ++i) {
            if (i > 0)
                ss << ", ";
            ss << formatRedisReply(reply->element[i]);
        }
        ss << "]";
        return ss.str();
    }
    case REDIS_REPLY_INTEGER:
        return std::to_string(reply->integer);
    case REDIS_REPLY_NIL:
        return std::string(NULL_SENTINEL);
    case REDIS_REPLY_STATUS:
        return {reply->str, reply->len};
    case REDIS_REPLY_ERROR:
        return "ERROR: " + std::string(reply->str, reply->len);
    default:
        return "UNKNOWN";
    }
}

std::string formatRedisPreview(redisReply* reply, std::string_view type) {
    if (!reply)
        return "[error]";

    if (type == "string") {
        return (reply->type == REDIS_REPLY_STRING) ? std::string(reply->str, reply->len) : "[nil]";
    }

    if (type == "list") {
        if (reply->type != REDIS_REPLY_ARRAY)
            return "[nil]";
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i < reply->elements && i < 5; ++i) {
            if (i > 0)
                ss << ", ";
            ss << "\"" << (reply->element[i]->str ? reply->element[i]->str : "") << "\"";
        }
        if (reply->elements > 5)
            ss << ", ...";
        ss << "]";
        return ss.str();
    }

    if (type == "set") {
        const redisReply* members = reply;
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
            reply->element[1]->type == REDIS_REPLY_ARRAY) {
            members = reply->element[1];
        }
        if (members->type != REDIS_REPLY_ARRAY)
            return "[nil]";
        std::stringstream ss;
        ss << "{";
        for (size_t i = 0; i < members->elements && i < 5; ++i) {
            if (i > 0)
                ss << ", ";
            ss << "\"" << (members->element[i]->str ? members->element[i]->str : "") << "\"";
        }
        if (members->elements > 5)
            ss << ", ...";
        ss << "}";
        return ss.str();
    }

    if (type == "hash") {
        const redisReply* pairs = reply;
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
            reply->element[1]->type == REDIS_REPLY_ARRAY) {
            pairs = reply->element[1];
        }
        if (pairs->type != REDIS_REPLY_ARRAY)
            return "[nil]";
        std::stringstream ss;
        ss << "{";
        for (size_t i = 0; i + 1 < pairs->elements && i < 10; i += 2) {
            if (i > 0)
                ss << ", ";
            ss << "\"" << (pairs->element[i]->str ? pairs->element[i]->str : "") << "\": \""
               << (pairs->element[i + 1]->str ? pairs->element[i + 1]->str : "") << "\"";
        }
        if (pairs->elements > 10)
            ss << ", ...";
        ss << "}";
        return ss.str();
    }

    if (type == "zset") {
        if (reply->type != REDIS_REPLY_ARRAY)
            return "[nil]";
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i + 1 < reply->elements && i < 10; i += 2) {
            if (i > 0)
                ss << ", ";
            ss << "\"" << (reply->element[i]->str ? reply->element[i]->str : "")
               << "\":" << (reply->element[i + 1]->str ? reply->element[i + 1]->str : "");
        }
        if (reply->elements > 10)
            ss << ", ...";
        ss << "]";
        return ss.str();
    }

    return "";
}

// Shared state between RedisConnection and the RedisDatabase view objects.
// One per RedisConnection. Guards the redisContext (commands aren't reentrant).
struct RedisState {
    std::mutex mu;
    redisContext* ctx = nullptr;
#if DEARSQL_HAS_HIREDIS_SSL
    redisSSLContext* ssl = nullptr;
#endif
    int selectedDb = 0;
    int numDatabases = 16;

    ~RedisState() {
        cleanup();
    }

    void cleanup() {
        if (ctx) {
            redisFree(ctx);
            ctx = nullptr;
        }
#if DEARSQL_HAS_HIREDIS_SSL
        if (ssl) {
            redisFreeSSLContext(ssl);
            ssl = nullptr;
        }
#endif
    }
};

// Per-logical-DB database view. Switches SELECT before each operation if the
// shared context's selected index differs.
class RedisDatabase final : public IDatabase {
public:
    RedisDatabase(std::shared_ptr<RedisState> state, int dbIndex, std::string connName)
        : state_(std::move(state)), index_(dbIndex), connName_(std::move(connName)) {}

    [[nodiscard]] std::string name() const override {
        return std::to_string(index_);
    }
    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::REDIS;
    }

    std::vector<Table> tables() override {
        std::vector<Table> out;
        if (!state_->ctx)
            return out;
        std::vector<std::string> keyNames;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            if (!ensureSelectedLocked())
                return out;
            keyNames = scanKeysLocked("*", 10000);
        }

        std::unordered_map<std::string, int> groups;
        for (const auto& k : keyNames) {
            auto pos = k.find(':');
            std::string prefix = (pos == std::string::npos) ? k : k.substr(0, pos);
            ++groups[prefix];
        }

        Table allKeys;
        allKeys.name = "*";
        allKeys.fullName = connName_ + ".db" + std::to_string(index_) + ".*";
        out.push_back(std::move(allKeys));

        std::vector<std::pair<std::string, int>> sorted(groups.begin(), groups.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [prefix, count] : sorted) {
            Table t;
            t.name = prefix + ":*";
            t.fullName = connName_ + ".db" + std::to_string(index_) + "." + t.name;
            out.push_back(std::move(t));
        }
        return out;
    }

    std::vector<Table> views() override {
        return {};
    }

    Table describeTable(const std::string& tableName) override {
        Table t;
        t.name = tableName;
        t.fullName = connName_ + ".db" + std::to_string(index_) + "." + tableName;
        Column c;
        c.name = "Key";
        c.type = "string";
        c.isPrimaryKey = true;
        c.isNotNull = true;
        t.columns.push_back(c);
        for (const char* col : {"Type", "Value", "TTL", "Size"}) {
            Column cc;
            cc.name = col;
            cc.type = "string";
            t.columns.push_back(cc);
        }
        return t;
    }

    QueryResult execute(const std::string& command, int /*rowLimit*/) override {
        QueryResult result;
        StatementResult s;
        const auto start = std::chrono::high_resolution_clock::now();

        auto parts = parseRedisCommand(command);
        if (parts.empty()) {
            s.success = false;
            s.errorMessage = "Empty command";
            result.statements.push_back(std::move(s));
            return result;
        }

        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->ctx) {
            s.success = false;
            s.errorMessage = "Not connected to Redis server";
            result.statements.push_back(std::move(s));
            return result;
        }
        if (!ensureSelectedLocked()) {
            s.success = false;
            s.errorMessage = std::format("Failed to switch to Redis database db{}", index_);
            result.statements.push_back(std::move(s));
            return result;
        }

        std::vector<const char*> argv;
        std::vector<size_t> argvlen;
        argv.reserve(parts.size());
        argvlen.reserve(parts.size());
        for (const auto& p : parts) {
            argv.push_back(p.c_str());
            argvlen.push_back(p.size());
        }

        auto reply = wrapReply(redisCommandArgv(state_->ctx, static_cast<int>(argv.size()),
                                                argv.data(), argvlen.data()));
        if (!reply) {
            s.success = false;
            s.errorMessage =
                state_->ctx->errstr[0] ? state_->ctx->errstr : "Failed to execute command";
        } else if (reply->type == REDIS_REPLY_ERROR) {
            s.success = false;
            s.errorMessage = std::string(reply->str, reply->len);
        } else {
            s.columnNames.push_back("result");
            s.tableData.push_back({formatRedisReply(reply.get())});
        }

        const auto end = std::chrono::high_resolution_clock::now();
        result.executionTimeMs = std::chrono::duration<double, std::milli>(end - start).count();
        result.statements.push_back(std::move(s));
        return result;
    }

    std::vector<std::vector<std::string>>
    getTableData(const Table& table, int limit, int offset, const std::string& /*whereClause*/,
                 const std::string& /*orderByClause*/) override {
        std::vector<std::vector<std::string>> data;
        const std::string pattern = table.name.empty() ? std::string("*") : table.name;
        std::vector<std::string> keyNames;
        {
            std::lock_guard<std::mutex> lock(state_->mu);
            if (!state_->ctx)
                return data;
            if (!ensureSelectedLocked())
                return data;
            keyNames = scanKeysLocked(pattern, limit + offset);
        }

        if (offset >= static_cast<int>(keyNames.size()))
            return data;

        std::sort(keyNames.begin(), keyNames.end());
        auto keys = fetchKeyDetails(keyNames);

        auto startIt = keys.begin() + offset;
        auto endIt = keys.begin() + std::min(offset + limit, static_cast<int>(keys.size()));
        for (auto it = startIt; it != endIt; ++it) {
            std::vector<std::string> row;
            row.push_back(it->name);
            row.push_back(it->type);
            row.push_back(it->value);
            row.push_back(std::to_string(it->ttl));
            if (it->size < 0)
                row.push_back("-");
            else if (it->size < 1024)
                row.push_back(std::to_string(it->size) + " B");
            else if (it->size < 1024 * 1024)
                row.push_back(std::format("{:.1f} KB", it->size / 1024.0));
            else
                row.push_back(std::format("{:.1f} MB", it->size / (1024.0 * 1024.0)));
            data.push_back(std::move(row));
        }
        return data;
    }

    std::vector<std::string> getColumnNames(const Table& /*table*/) override {
        return {"Key", "Type", "Value", "TTL", "Size"};
    }

    int getRowCount(const Table& table, const std::string& /*whereClause*/) override {
        const std::string pattern = table.name.empty() ? std::string("*") : table.name;
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->ctx)
            return 0;
        if (!ensureSelectedLocked())
            return 0;
        return static_cast<int>(scanKeysLocked(pattern, 10000).size());
    }

private:
    // requires state_->mu held
    bool ensureSelectedLocked() {
        if (!state_->ctx)
            return false;
        if (state_->selectedDb == index_)
            return true;
        auto reply = wrapReply(redisCommand(state_->ctx, "SELECT %d", index_));
        if (!reply || reply->type == REDIS_REPLY_ERROR)
            return false;
        state_->selectedDb = index_;
        return true;
    }

    // requires state_->mu held
    std::vector<std::string> scanKeysLocked(const std::string& pattern, int limit) {
        std::vector<std::string> keys;
        if (!state_->ctx)
            return keys;
        unsigned long long cursor = 0;
        do {
            auto reply = wrapReply(redisCommand(state_->ctx, "SCAN %llu MATCH %s COUNT 200", cursor,
                                                pattern.c_str()));
            if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2)
                break;
            cursor = std::strtoull(reply->element[0]->str, nullptr, 10);
            auto* keysArray = reply->element[1];
            for (size_t i = 0; i < keysArray->elements; ++i) {
                if (keysArray->element[i]->type == REDIS_REPLY_STRING) {
                    keys.emplace_back(keysArray->element[i]->str, keysArray->element[i]->len);
                    if (static_cast<int>(keys.size()) >= limit) {
                        cursor = 0;
                        break;
                    }
                }
            }
        } while (cursor != 0);
        return keys;
    }

    // Acquires state_->mu internally. Pipelines TYPE/TTL/MEMORY USAGE + per-type
    // preview commands using append/get-reply.
    std::vector<RedisKey> fetchKeyDetails(const std::vector<std::string>& keyNames) {
        std::vector<RedisKey> keys(keyNames.size());
        std::lock_guard<std::mutex> lock(state_->mu);
        if (!state_->ctx)
            return keys;
        if (!ensureSelectedLocked())
            return keys;

        for (const auto& n : keyNames) {
            redisAppendCommand(state_->ctx, "TYPE %s", n.c_str());
            redisAppendCommand(state_->ctx, "TTL %s", n.c_str());
            redisAppendCommand(state_->ctx, "MEMORY USAGE %s", n.c_str());
        }
        for (size_t i = 0; i < keyNames.size(); ++i) {
            keys[i].name = keyNames[i];

            redisReply* raw = nullptr;
            if (redisGetReply(state_->ctx, reinterpret_cast<void**>(&raw)) == REDIS_OK && raw) {
                RedisReplyPtr typeReply(raw);
                if (typeReply->type == REDIS_REPLY_STATUS)
                    keys[i].type = typeReply->str;
                else
                    keys[i].type = "unknown";
            } else {
                keys[i].type = "unknown";
            }

            raw = nullptr;
            if (redisGetReply(state_->ctx, reinterpret_cast<void**>(&raw)) == REDIS_OK && raw) {
                RedisReplyPtr ttlReply(raw);
                if (ttlReply->type == REDIS_REPLY_INTEGER)
                    keys[i].ttl = ttlReply->integer;
            }

            raw = nullptr;
            if (redisGetReply(state_->ctx, reinterpret_cast<void**>(&raw)) == REDIS_OK && raw) {
                RedisReplyPtr memReply(raw);
                if (memReply->type == REDIS_REPLY_INTEGER)
                    keys[i].size = memReply->integer;
            }
        }

        for (const auto& key : keys) {
            if (key.type == "string")
                redisAppendCommand(state_->ctx, "GET %s", key.name.c_str());
            else if (key.type == "list")
                redisAppendCommand(state_->ctx, "LRANGE %s 0 4", key.name.c_str());
            else if (key.type == "set")
                redisAppendCommand(state_->ctx, "SSCAN %s 0 COUNT 5", key.name.c_str());
            else if (key.type == "zset")
                redisAppendCommand(state_->ctx, "ZRANGE %s 0 4 WITHSCORES", key.name.c_str());
            else if (key.type == "hash")
                redisAppendCommand(state_->ctx, "HSCAN %s 0 COUNT 5", key.name.c_str());
            else
                redisAppendCommand(state_->ctx, "TYPE %s", key.name.c_str());
        }
        for (size_t i = 0; i < keys.size(); ++i) {
            redisReply* raw = nullptr;
            if (redisGetReply(state_->ctx, reinterpret_cast<void**>(&raw)) != REDIS_OK || !raw) {
                keys[i].value = "[error]";
                continue;
            }
            RedisReplyPtr valReply(raw);
            keys[i].value = formatRedisPreview(valReply.get(), keys[i].type);
        }
        return keys;
    }

    std::shared_ptr<RedisState> state_;
    int index_;
    std::string connName_;
};

struct RedisConnectionImpl {
    std::shared_ptr<RedisState> state = std::make_shared<RedisState>();
    std::unordered_map<int, std::shared_ptr<RedisDatabase>> dbCache;
};

RedisConnectionImpl* asImpl(void* p) {
    return static_cast<RedisConnectionImpl*>(p);
}
const RedisConnectionImpl* asImpl(const void* p) {
    return static_cast<const RedisConnectionImpl*>(p);
}

} // namespace

RedisConnection::RedisConnection(const ConnectionInfo& info) : info_(info) {
    impl_ = new RedisConnectionImpl();
}

RedisConnection::~RedisConnection() {
    RedisConnection::close();
    delete asImpl(impl_);
    impl_ = nullptr;
}

Status RedisConnection::open() {
    auto* im = asImpl(impl_);
    auto& st = *im->state;
    std::lock_guard<std::mutex> lock(st.mu);

    if (st.ctx)
        return {true, ""};

    constexpr timeval timeout = {5, 0};
    st.ctx = redisConnectWithTimeout(info_.host.c_str(), info_.port, timeout);
    if (!st.ctx || st.ctx->err) {
        std::string err = st.ctx ? st.ctx->errstr : "Failed to allocate redis context";
        st.cleanup();
        return {false, err};
    }

#if DEARSQL_HAS_HIREDIS_SSL
    if (info_.sslmode == SslMode::Require || info_.sslmode == SslMode::VerifyCA ||
        info_.sslmode == SslMode::VerifyFull) {
        redisSSLContextError sslErr = REDIS_SSL_CTX_NONE;
        const char* caPath = !info_.sslCACertPath.empty() ? info_.sslCACertPath.c_str() : nullptr;
        st.ssl = redisCreateSSLContext(caPath, nullptr, nullptr, nullptr, nullptr, &sslErr);
        if (!st.ssl) {
            std::string err =
                std::string("Redis SSL context failed: ") + redisSSLContextGetError(sslErr);
            st.cleanup();
            return {false, err};
        }
        if (redisInitiateSSLWithContext(st.ctx, st.ssl) != REDIS_OK) {
            std::string err = st.ctx->errstr[0] ? st.ctx->errstr : "Redis TLS handshake failed";
            st.cleanup();
            return {false, err};
        }
    }
#endif

    if (!info_.password.empty()) {
        RedisReplyPtr reply;
        if (!info_.username.empty()) {
            reply = wrapReply(redisCommand(st.ctx, "AUTH %s %s", info_.username.c_str(),
                                           info_.password.c_str()));
        } else {
            reply = wrapReply(redisCommand(st.ctx, "AUTH %s", info_.password.c_str()));
        }
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            std::string err =
                reply ? std::string(reply->str, reply->len) : "Authentication failed";
            st.cleanup();
            return {false, err};
        }
    }

    {
        auto reply = wrapReply(redisCommand(st.ctx, "PING"));
        if (!reply || reply->type == REDIS_REPLY_ERROR) {
            std::string err =
                reply ? std::string(reply->str, reply->len) : "Connection test failed";
            st.cleanup();
            return {false, err};
        }
    }

    // discover configured DB count
    st.numDatabases = 16;
    {
        auto reply = wrapReply(redisCommand(st.ctx, "CONFIG GET databases"));
        if (reply && reply->type == REDIS_REPLY_ARRAY && reply->elements == 2 &&
            reply->element[1]->type == REDIS_REPLY_STRING) {
            const int n = std::atoi(reply->element[1]->str);
            if (n > 0)
                st.numDatabases = n;
        }
    }

    st.selectedDb = 0;
    selectedDb_ = 0;

    // honor requested logical database via info_.database (e.g. "1")
    if (!info_.database.empty()) {
        try {
            int idx = std::stoi(info_.database);
            if (idx >= 0 && idx < st.numDatabases) {
                auto reply = wrapReply(redisCommand(st.ctx, "SELECT %d", idx));
                if (reply && reply->type != REDIS_REPLY_ERROR) {
                    st.selectedDb = idx;
                    selectedDb_ = idx;
                }
            }
        } catch (...) {
        }
    }
    return {true, ""};
}

void RedisConnection::close() {
    auto* im = asImpl(impl_);
    if (!im)
        return;
    im->dbCache.clear();
    std::lock_guard<std::mutex> lock(im->state->mu);
    im->state->cleanup();
    im->state->selectedDb = 0;
}

bool RedisConnection::isOpen() const {
    const auto* im = asImpl(impl_);
    if (!im)
        return false;
    std::lock_guard<std::mutex> lock(im->state->mu);
    return im->state->ctx != nullptr;
}

std::vector<DatabasePtr> RedisConnection::databases() {
    auto* im = asImpl(impl_);
    std::vector<DatabasePtr> out;
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(im->state->mu);
        if (!im->state->ctx)
            return out;
        count = im->state->numDatabases;
    }
    out.reserve(count);
    for (int i = 0; i < count; ++i)
        out.push_back(database(std::to_string(i)));
    return out;
}

DatabasePtr RedisConnection::database(const std::string& name) {
    auto* im = asImpl(impl_);
    int idx = selectedDb_;
    if (!name.empty()) {
        try {
            idx = std::stoi(name);
        } catch (...) {
            return nullptr;
        }
    }
    auto it = im->dbCache.find(idx);
    if (it != im->dbCache.end())
        return it->second;
    auto db = std::make_shared<RedisDatabase>(im->state, idx, info_.name);
    im->dbCache[idx] = db;
    return db;
}

std::vector<RedisKey> RedisConnection::getKeys(const std::string& pattern, int limit) {
    std::vector<RedisKey> keys;
    auto* im = asImpl(impl_);
    std::lock_guard<std::mutex> lock(im->state->mu);
    if (!im->state->ctx)
        return keys;

    std::vector<std::string> keyNames;
    unsigned long long cursor = 0;
    do {
        auto reply = wrapReply(redisCommand(im->state->ctx, "SCAN %llu MATCH %s COUNT 200", cursor,
                                            pattern.c_str()));
        if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2)
            break;
        cursor = std::strtoull(reply->element[0]->str, nullptr, 10);
        auto* keysArray = reply->element[1];
        for (size_t i = 0; i < keysArray->elements; ++i) {
            if (keysArray->element[i]->type == REDIS_REPLY_STRING) {
                keyNames.emplace_back(keysArray->element[i]->str, keysArray->element[i]->len);
                if (static_cast<int>(keyNames.size()) >= limit) {
                    cursor = 0;
                    break;
                }
            }
        }
    } while (cursor != 0);

    std::sort(keyNames.begin(), keyNames.end());

    for (const auto& n : keyNames) {
        redisAppendCommand(im->state->ctx, "TYPE %s", n.c_str());
        redisAppendCommand(im->state->ctx, "TTL %s", n.c_str());
        redisAppendCommand(im->state->ctx, "MEMORY USAGE %s", n.c_str());
    }
    keys.resize(keyNames.size());
    for (size_t i = 0; i < keyNames.size(); ++i) {
        keys[i].name = keyNames[i];

        redisReply* raw = nullptr;
        if (redisGetReply(im->state->ctx, reinterpret_cast<void**>(&raw)) == REDIS_OK && raw) {
            RedisReplyPtr typeReply(raw);
            if (typeReply->type == REDIS_REPLY_STATUS)
                keys[i].type = typeReply->str;
            else
                keys[i].type = "unknown";
        } else {
            keys[i].type = "unknown";
        }

        raw = nullptr;
        if (redisGetReply(im->state->ctx, reinterpret_cast<void**>(&raw)) == REDIS_OK && raw) {
            RedisReplyPtr ttlReply(raw);
            if (ttlReply->type == REDIS_REPLY_INTEGER)
                keys[i].ttl = ttlReply->integer;
        }

        raw = nullptr;
        if (redisGetReply(im->state->ctx, reinterpret_cast<void**>(&raw)) == REDIS_OK && raw) {
            RedisReplyPtr memReply(raw);
            if (memReply->type == REDIS_REPLY_INTEGER)
                keys[i].size = memReply->integer;
        }
    }

    for (const auto& key : keys) {
        if (key.type == "string")
            redisAppendCommand(im->state->ctx, "GET %s", key.name.c_str());
        else if (key.type == "list")
            redisAppendCommand(im->state->ctx, "LRANGE %s 0 4", key.name.c_str());
        else if (key.type == "set")
            redisAppendCommand(im->state->ctx, "SSCAN %s 0 COUNT 5", key.name.c_str());
        else if (key.type == "zset")
            redisAppendCommand(im->state->ctx, "ZRANGE %s 0 4 WITHSCORES", key.name.c_str());
        else if (key.type == "hash")
            redisAppendCommand(im->state->ctx, "HSCAN %s 0 COUNT 5", key.name.c_str());
        else
            redisAppendCommand(im->state->ctx, "TYPE %s", key.name.c_str());
    }
    for (size_t i = 0; i < keys.size(); ++i) {
        redisReply* raw = nullptr;
        if (redisGetReply(im->state->ctx, reinterpret_cast<void**>(&raw)) != REDIS_OK || !raw) {
            keys[i].value = "[error]";
            continue;
        }
        RedisReplyPtr valReply(raw);
        keys[i].value = formatRedisPreview(valReply.get(), keys[i].type);
    }
    return keys;
}

std::string RedisConnection::getKeyValue(const std::string& key, const std::string& knownType) {
    std::string type = knownType.empty() ? getKeyType(key) : knownType;

    auto* im = asImpl(impl_);
    std::lock_guard<std::mutex> lock(im->state->mu);
    if (!im->state->ctx)
        return "[Unable to retrieve value]";

    RedisReplyPtr reply;
    if (type == "string")
        reply = wrapReply(redisCommand(im->state->ctx, "GET %s", key.c_str()));
    else if (type == "list")
        reply = wrapReply(redisCommand(im->state->ctx, "LRANGE %s 0 4", key.c_str()));
    else if (type == "set")
        reply = wrapReply(redisCommand(im->state->ctx, "SMEMBERS %s", key.c_str()));
    else if (type == "hash")
        reply = wrapReply(redisCommand(im->state->ctx, "HGETALL %s", key.c_str()));
    else if (type == "zset")
        reply = wrapReply(redisCommand(im->state->ctx, "ZRANGE %s 0 4 WITHSCORES", key.c_str()));

    if (reply) {
        std::string value = formatRedisPreview(reply.get(), type);
        if (!value.empty())
            return value;
    }
    return "[Unable to retrieve value]";
}

std::string RedisConnection::getKeyType(const std::string& key) {
    auto* im = asImpl(impl_);
    std::lock_guard<std::mutex> lock(im->state->mu);
    if (!im->state->ctx)
        return "unknown";
    auto reply = wrapReply(redisCommand(im->state->ctx, "TYPE %s", key.c_str()));
    if (reply && reply->type == REDIS_REPLY_STATUS)
        return reply->str;
    return "unknown";
}

int64_t RedisConnection::getKeyTTL(const std::string& key) {
    auto* im = asImpl(impl_);
    std::lock_guard<std::mutex> lock(im->state->mu);
    if (!im->state->ctx)
        return -1;
    auto reply = wrapReply(redisCommand(im->state->ctx, "TTL %s", key.c_str()));
    if (reply && reply->type == REDIS_REPLY_INTEGER)
        return reply->integer;
    return -1;
}

bool RedisConnection::selectDatabase(int dbIndex) {
    auto* im = asImpl(impl_);
    std::lock_guard<std::mutex> lock(im->state->mu);
    if (!im->state->ctx)
        return false;
    if (dbIndex < 0 || dbIndex >= im->state->numDatabases)
        return false;
    auto reply = wrapReply(redisCommand(im->state->ctx, "SELECT %d", dbIndex));
    if (!reply || reply->type == REDIS_REPLY_ERROR)
        return false;
    im->state->selectedDb = dbIndex;
    selectedDb_ = dbIndex;
    return true;
}

} // namespace dearsql
