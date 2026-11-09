#include "dearsql/backends/mongodb_connection.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <bson/bson.h>
#include <mongoc/mongoc.h>

namespace dearsql {

namespace {

// mongoc_init once per process; never cleaned up (other threads may still use it)
void initDriver() {
    static std::once_flag once;
    std::call_once(once, [] { mongoc_init(); });
}

constexpr uint32_t kNamespaceNotFound = 26;

[[noreturn]] void fail(const bson_error_t& error) {
    throw Error(error.message);
}

struct BsonDeleter {
    void operator()(bson_t* b) const {
        bson_destroy(b);
    }
};
using BsonPtr = std::unique_ptr<bson_t, BsonDeleter>;

// a stack bson_t, destroyed on scope exit. driver out-params (replies) re-init it.
struct Bson {
    bson_t b;
    Bson() {
        bson_init(&b);
    }
    ~Bson() {
        bson_destroy(&b);
    }
    Bson(const Bson&) = delete;
    Bson& operator=(const Bson&) = delete;
    bson_t* get() {
        return &b;
    }
};

BsonPtr fromJson(const std::string& json, bson_error_t* error) {
    return BsonPtr(bson_new_from_json(reinterpret_cast<const uint8_t*>(json.data()),
                                      static_cast<ssize_t>(json.size()), error));
}

// same text bsoncxx::to_json gave (its default mode is legacy)
std::string toJson(const bson_t* doc) {
    size_t len = 0;
    char* s = bson_as_legacy_extended_json(doc, &len);
    if (!s)
        return "";
    std::string out(s, len);
    bson_free(s);
    return out;
}

std::string arrayToJson(const bson_t* arr) {
    size_t len = 0;
    char* s = bson_array_as_legacy_extended_json(arr, &len);
    if (!s)
        return "";
    std::string out(s, len);
    bson_free(s);
    return out;
}

// view of an embedded document or array as a static bson_t
bool subDocument(const bson_iter_t* it, bson_t* out) {
    uint32_t len = 0;
    const uint8_t* data = nullptr;
    if (BSON_ITER_HOLDS_DOCUMENT(it))
        bson_iter_document(it, &len, &data);
    else if (BSON_ITER_HOLDS_ARRAY(it))
        bson_iter_array(it, &len, &data);
    else
        return false;
    return bson_init_static(out, data, len);
}

std::string utf8(const bson_iter_t* it) {
    uint32_t len = 0;
    const char* s = bson_iter_utf8(it, &len);
    return s ? std::string(s, len) : std::string();
}

// string field of a document, if present and a string
bool findString(const bson_t* doc, const char* key, std::string& out) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, doc, key) || !BSON_ITER_HOLDS_UTF8(&it))
        return false;
    out = utf8(&it);
    return true;
}

// embedded document (or array) field, viewed in place
bool findDocument(const bson_t* doc, const char* key, bson_t* out) {
    bson_iter_t it;
    if (!bson_iter_init_find(&it, doc, key))
        return false;
    return subDocument(&it, out);
}

bool hasField(const bson_t* doc, const char* key) {
    bson_iter_t it;
    return bson_iter_init_find(&it, doc, key);
}

int64_t findInt(const bson_t* doc, const char* key) {
    bson_iter_t it;
    if (bson_iter_init_find(&it, doc, key) && BSON_ITER_HOLDS_NUMBER(&it))
        return bson_iter_as_int64(&it);
    return 0;
}

std::string elementToString(const bson_iter_t* it) {
    switch (bson_iter_type(it)) {
    case BSON_TYPE_UTF8:
        return utf8(it);
    case BSON_TYPE_INT32:
        return std::to_string(bson_iter_int32(it));
    case BSON_TYPE_INT64:
        return std::to_string(bson_iter_int64(it));
    case BSON_TYPE_DOUBLE:
        return std::to_string(bson_iter_double(it));
    case BSON_TYPE_BOOL:
        return bson_iter_bool(it) ? std::string(BOOL_TRUE_SENTINEL)
                                  : std::string(BOOL_FALSE_SENTINEL);
    case BSON_TYPE_OID: {
        char buf[25];
        bson_oid_to_string(bson_iter_oid(it), buf);
        return buf;
    }
    case BSON_TYPE_DATE_TIME: {
        const auto millis = bson_iter_date_time(it);
        const auto time = static_cast<std::time_t>(millis / 1000);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }
    case BSON_TYPE_NULL:
        return std::string(NULL_SENTINEL);
    case BSON_TYPE_DECIMAL128: {
        bson_decimal128_t dec;
        bson_iter_decimal128(it, &dec);
        char buf[BSON_DECIMAL128_STRING];
        bson_decimal128_to_string(&dec, buf);
        return buf;
    }
    case BSON_TYPE_DOCUMENT: {
        bson_t sub;
        return subDocument(it, &sub) ? toJson(&sub) : "";
    }
    case BSON_TYPE_ARRAY: {
        bson_t sub;
        return subDocument(it, &sub) ? arrayToJson(&sub) : "";
    }
    default: {
        // anything else as {"v": <extended json>}
        Bson wrapper;
        if (!bson_append_value(wrapper.get(), "v", 1, bson_iter_value(const_cast<bson_iter_t*>(it))))
            return "<unknown>";
        return toJson(wrapper.get());
    }
    }
}

const char* typeName(bson_type_t type) {
    switch (type) {
    case BSON_TYPE_DOUBLE:
        return "double";
    case BSON_TYPE_UTF8:
        return "string";
    case BSON_TYPE_DOCUMENT:
        return "object";
    case BSON_TYPE_ARRAY:
        return "array";
    case BSON_TYPE_BINARY:
        return "binary";
    case BSON_TYPE_OID:
        return "objectId";
    case BSON_TYPE_BOOL:
        return "bool";
    case BSON_TYPE_DATE_TIME:
        return "date";
    case BSON_TYPE_NULL:
        return "null";
    case BSON_TYPE_INT32:
        return "int32";
    case BSON_TYPE_INT64:
        return "int64";
    case BSON_TYPE_DECIMAL128:
        return "decimal128";
    default:
        return "unknown";
    }
}

using PoolPtr = std::shared_ptr<mongoc_client_pool_t>;

struct DatabaseDeleter {
    void operator()(mongoc_database_t* d) const {
        mongoc_database_destroy(d);
    }
};
struct CollectionDeleter {
    void operator()(mongoc_collection_t* c) const {
        mongoc_collection_destroy(c);
    }
};
struct CursorDeleter {
    void operator()(mongoc_cursor_t* c) const {
        mongoc_cursor_destroy(c);
    }
};
using DatabaseHandle = std::unique_ptr<mongoc_database_t, DatabaseDeleter>;
using CollectionHandle = std::unique_ptr<mongoc_collection_t, CollectionDeleter>;
using CursorHandle = std::unique_ptr<mongoc_cursor_t, CursorDeleter>;

// a client popped from the pool, pushed back on scope exit. declare database and
// collection handles after it so they are destroyed first.
class Client {
public:
    explicit Client(PoolPtr pool)
        : pool_(std::move(pool)), client_(mongoc_client_pool_pop(pool_.get())) {}
    ~Client() {
        if (client_)
            mongoc_client_pool_push(pool_.get(), client_);
    }
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    DatabaseHandle database(const std::string& name) const {
        return DatabaseHandle(mongoc_client_get_database(client_, name.c_str()));
    }
    CollectionHandle collection(const std::string& db, const std::string& coll) const {
        return CollectionHandle(mongoc_client_get_collection(client_, db.c_str(), coll.c_str()));
    }
    std::vector<std::string> databaseNames() const {
        bson_error_t error;
        char** names = mongoc_client_get_database_names_with_opts(client_, nullptr, &error);
        if (!names)
            fail(error);
        std::vector<std::string> out;
        for (char** n = names; *n; ++n)
            out.emplace_back(*n);
        bson_strfreev(names);
        return out;
    }

private:
    PoolPtr pool_;
    mongoc_client_t* client_;
};

// runs f on each document; a cursor error throws, as mongocxx did
template <typename F> void forEach(mongoc_cursor_t* cursor, F&& f) {
    const bson_t* doc = nullptr;
    while (mongoc_cursor_next(cursor, &doc))
        f(doc);
    bson_error_t error;
    if (mongoc_cursor_error(cursor, &error))
        fail(error);
}

// drop that, like mongocxx, ignores a missing collection
bool dropCollection(mongoc_collection_t* coll, bson_error_t* error) {
    return mongoc_collection_drop(coll, error) || error->code == kNamespaceNotFound;
}

std::string pluralDocs(size_t n) {
    return n == 1 ? "" : "s";
}

// Per-database handle. Holds a shared_ptr to the pool so the pool outlives any
// database handle handed out by the connection.
class MongoDBDatabase final : public IDatabase {
public:
    MongoDBDatabase(PoolPtr pool, std::string connName, std::string dbName)
        : pool_(std::move(pool)), connName_(std::move(connName)), name_(std::move(dbName)) {}

    [[nodiscard]] std::string name() const override {
        return name_;
    }
    [[nodiscard]] DatabaseType type() const override {
        return DatabaseType::MONGODB;
    }

    std::vector<Table> tables() override;
    std::vector<Table> views() override {
        return {};
    }
    Table describeTable(const std::string& tableName) override;

    QueryResult execute(const std::string& sql, int rowLimit) override;

    std::vector<std::vector<std::string>> getTableData(const Table& table, int limit, int offset,
                                                       const std::string& whereClause,
                                                       const std::string& orderByClause) override;
    std::vector<std::string> getColumnNames(const Table& table) override;
    int getRowCount(const Table& table, const std::string& whereClause) override;

    Status createTable(const Table& table) override;
    Status dropTable(const std::string& tableName) override;

private:
    std::vector<Column> inferSchemaFromSample(const Client& client,
                                              const std::string& collectionName, int sampleSize);
    std::vector<Index> getCollectionIndexes(const Client& client,
                                            const std::string& collectionName);
    void executeWrapper(const Client& client, const bson_t* cmd, int rowLimit,
                        StatementResult& s);

    PoolPtr pool_;
    std::string connName_;
    std::string name_;
};

std::vector<Table> MongoDBDatabase::tables() {
    std::vector<Table> result;
    if (!pool_)
        return result;
    Client client(pool_);
    std::vector<std::string> names;
    {
        auto db = client.database(name_);
        bson_error_t error;
        char** raw = mongoc_database_get_collection_names_with_opts(db.get(), nullptr, &error);
        if (!raw)
            fail(error);
        for (char** n = raw; *n; ++n)
            names.emplace_back(*n);
        bson_strfreev(raw);
    }
    for (const auto& collName : names) {
        if (collName.starts_with("system."))
            continue;
        Table t;
        t.name = collName;
        t.fullName = connName_ + "." + name_ + "." + collName;
        t.columns = inferSchemaFromSample(client, collName, 100);
        t.indexes = getCollectionIndexes(client, collName);
        result.push_back(std::move(t));
    }
    return result;
}

Table MongoDBDatabase::describeTable(const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.fullName = connName_ + "." + name_ + "." + tableName;
    if (!pool_)
        return t;
    Client client(pool_);
    t.columns = inferSchemaFromSample(client, tableName, 100);
    t.indexes = getCollectionIndexes(client, tableName);
    return t;
}

std::vector<Column> MongoDBDatabase::inferSchemaFromSample(const Client& client,
                                                           const std::string& collectionName,
                                                           int sampleSize) {
    std::vector<Column> columns;
    std::set<std::string> seenFields;
    try {
        auto coll = client.collection(name_, collectionName);
        Bson filter, opts;
        BSON_APPEND_INT64(opts.get(), "limit", sampleSize);
        CursorHandle cursor(
            mongoc_collection_find_with_opts(coll.get(), filter.get(), opts.get(), nullptr));
        forEach(cursor.get(), [&](const bson_t* doc) {
            bson_iter_t it;
            if (!bson_iter_init(&it, doc))
                return;
            while (bson_iter_next(&it)) {
                std::string fieldName = bson_iter_key(&it);
                if (!seenFields.insert(fieldName).second)
                    continue;
                Column col;
                col.name = fieldName;
                col.type = typeName(bson_iter_type(&it));
                if (fieldName == "_id") {
                    col.isPrimaryKey = true;
                    col.isNotNull = true;
                }
                columns.push_back(std::move(col));
            }
        });
        std::ranges::sort(columns, [](const Column& a, const Column& b) {
            if (a.name == "_id")
                return true;
            if (b.name == "_id")
                return false;
            return a.name < b.name;
        });
    } catch (...) {
    }
    return columns;
}

std::vector<Index> MongoDBDatabase::getCollectionIndexes(const Client& client,
                                                         const std::string& collectionName) {
    std::vector<Index> indexes;
    try {
        auto coll = client.collection(name_, collectionName);
        CursorHandle cursor(mongoc_collection_find_indexes_with_opts(coll.get(), nullptr));
        forEach(cursor.get(), [&](const bson_t* doc) {
            Index idx;
            findString(doc, "name", idx.name);
            bson_t key;
            bson_iter_t it;
            if (bson_iter_init_find(&it, doc, "key") && BSON_ITER_HOLDS_DOCUMENT(&it) &&
                subDocument(&it, &key)) {
                bson_iter_t field;
                if (bson_iter_init(&field, &key))
                    while (bson_iter_next(&field))
                        idx.columns.emplace_back(bson_iter_key(&field));
            }
            if (bson_iter_init_find(&it, doc, "unique") && BSON_ITER_HOLDS_BOOL(&it))
                idx.isUnique = bson_iter_bool(&it);
            if (idx.name == "_id_") {
                idx.isPrimary = true;
                idx.isUnique = true;
            }
            indexes.push_back(std::move(idx));
        });
    } catch (...) {
    }
    return indexes;
}

// Execute a JSON command string. Two protocols are accepted:
//   1) Wrapper form: {"database":"...","collection":"...","command":"find",...}
//   2) Native MongoDB command (passed straight to runCommand), e.g.
//      {"insert":"coll","documents":[{...}]} or {"find":"coll","filter":{...}}.
// Detection: if the parsed doc has a top-level "command" string, use form (1);
// otherwise treat it as a native runCommand body.
QueryResult MongoDBDatabase::execute(const std::string& sql, int rowLimit) {
    QueryResult result;
    StatementResult s;
    const auto startTime = std::chrono::high_resolution_clock::now();
    const auto finish = [&] {
        const auto endTime = std::chrono::high_resolution_clock::now();
        result.executionTimeMs =
            std::chrono::duration<double, std::milli>(endTime - startTime).count();
        result.statements.push_back(std::move(s));
        return result;
    };
    if (!pool_) {
        s.success = false;
        s.errorMessage = "Not connected";
        result.statements.push_back(std::move(s));
        return result;
    }

    bson_error_t parseError;
    BsonPtr doc = fromJson(sql, &parseError);
    if (!doc) {
        s.success = false;
        s.errorMessage = std::string("Invalid JSON command: ") + parseError.message;
        return finish();
    }

    try {
        Client client(pool_);
        std::string command;
        if (findString(doc.get(), "command", command)) {
            executeWrapper(client, doc.get(), rowLimit, s);
        } else {
            auto db = client.database(name_);
            Bson reply;
            bson_error_t error;
            if (!mongoc_database_command_with_opts(db.get(), doc.get(), nullptr, nullptr,
                                                   reply.get(), &error))
                fail(error);
            s.columnNames.push_back("result");
            s.tableData.push_back({toJson(reply.get())});
            s.message = "Command executed successfully";
        }
    } catch (const std::exception& e) {
        s.success = false;
        s.errorMessage = e.what();
    }
    return finish();
}

void MongoDBDatabase::executeWrapper(const Client& client, const bson_t* cmd, int rowLimit,
                                     StatementResult& s) {
    std::string dbName = name_;
    std::string collName;
    std::string command;
    findString(cmd, "database", dbName);
    findString(cmd, "collection", collName);
    findString(cmd, "command", command);

    const auto requireDocument = [&](const char* key, bson_t* out) {
        if (!findDocument(cmd, key, out))
            throw Error(std::format("\"{}\" must be a document", key));
    };
    bson_error_t error;

    if (command == "find" && !collName.empty()) {
        auto coll = client.collection(dbName, collName);
        Bson empty, opts;
        bson_t filterDoc;
        const bson_t* filter = empty.get();
        if (hasField(cmd, "filter")) {
            requireDocument("filter", &filterDoc);
            filter = &filterDoc;
        }
        if (rowLimit > 0)
            BSON_APPEND_INT64(opts.get(), "limit", rowLimit);
        CursorHandle cursor(
            mongoc_collection_find_with_opts(coll.get(), filter, opts.get(), nullptr));
        s.columnNames.push_back("_id");
        s.columnNames.push_back("document");
        forEach(cursor.get(), [&](const bson_t* d) {
            std::vector<std::string> row;
            bson_iter_t it;
            if (bson_iter_init_find(&it, d, "_id")) {
                Bson idDoc;
                bson_append_value(idDoc.get(), "_id", 3, bson_iter_value(&it));
                row.push_back(toJson(idDoc.get()));
            } else {
                row.push_back("");
            }
            row.push_back(toJson(d));
            s.tableData.push_back(std::move(row));
        });
        s.message =
            std::format("Returned {} document{}", s.tableData.size(), pluralDocs(s.tableData.size()));
    } else if (command == "aggregate" && !collName.empty()) {
        auto coll = client.collection(dbName, collName);
        Bson empty;
        bson_t stages;
        const bson_t* pipeline = empty.get();
        if (hasField(cmd, "pipeline")) {
            bson_iter_t it;
            if (!bson_iter_init_find(&it, cmd, "pipeline") || !BSON_ITER_HOLDS_ARRAY(&it) ||
                !subDocument(&it, &stages))
                throw Error("\"pipeline\" must be an array");
            pipeline = &stages;
        }
        CursorHandle cursor(mongoc_collection_aggregate(coll.get(), MONGOC_QUERY_NONE, pipeline,
                                                        nullptr, nullptr));
        s.columnNames.push_back("document");
        forEach(cursor.get(), [&](const bson_t* d) { s.tableData.push_back({toJson(d)}); });
        s.message =
            std::format("Returned {} document{}", s.tableData.size(), pluralDocs(s.tableData.size()));
    } else if (command == "insert" && !collName.empty()) {
        auto coll = client.collection(dbName, collName);
        if (hasField(cmd, "document")) {
            bson_t one;
            requireDocument("document", &one);
            Bson reply;
            if (!mongoc_collection_insert_one(coll.get(), &one, nullptr, reply.get(), &error))
                fail(error);
            s.affectedRows = 1;
        } else if (hasField(cmd, "documents")) {
            bson_iter_t it;
            bson_t arr;
            if (!bson_iter_init_find(&it, cmd, "documents") || !BSON_ITER_HOLDS_ARRAY(&it) ||
                !subDocument(&it, &arr))
                throw Error("\"documents\" must be an array");
            std::vector<bson_t> docs;
            bson_iter_t elem;
            if (bson_iter_init(&elem, &arr))
                while (bson_iter_next(&elem)) {
                    bson_t d;
                    if (!BSON_ITER_HOLDS_DOCUMENT(&elem) || !subDocument(&elem, &d))
                        throw Error("\"documents\" must hold documents");
                    docs.push_back(d);
                }
            std::vector<const bson_t*> ptrs;
            for (const auto& d : docs)
                ptrs.push_back(&d);
            Bson reply;
            if (!mongoc_collection_insert_many(coll.get(), ptrs.data(), ptrs.size(), nullptr,
                                               reply.get(), &error))
                fail(error);
            s.affectedRows = static_cast<int>(findInt(reply.get(), "insertedCount"));
        }
        s.message = "Insert executed successfully";
    } else if (command == "update" && !collName.empty()) {
        auto coll = client.collection(dbName, collName);
        bson_t filter, update;
        requireDocument("filter", &filter);
        requireDocument("update", &update);
        Bson reply;
        if (!mongoc_collection_update_many(coll.get(), &filter, &update, nullptr, reply.get(),
                                           &error))
            fail(error);
        s.affectedRows = static_cast<int>(findInt(reply.get(), "modifiedCount"));
        s.message = std::format("Updated {} document{}", s.affectedRows,
                                s.affectedRows == 1 ? "" : "s");
    } else if (command == "delete" && !collName.empty()) {
        auto coll = client.collection(dbName, collName);
        bson_t filter;
        requireDocument("filter", &filter);
        Bson reply;
        if (!mongoc_collection_delete_many(coll.get(), &filter, nullptr, reply.get(), &error))
            fail(error);
        s.affectedRows = static_cast<int>(findInt(reply.get(), "deletedCount"));
        s.message = std::format("Deleted {} document{}", s.affectedRows,
                                s.affectedRows == 1 ? "" : "s");
    } else if (command == "createCollection" && !collName.empty()) {
        auto db = client.database(dbName);
        CollectionHandle created(
            mongoc_database_create_collection(db.get(), collName.c_str(), nullptr, &error));
        if (!created)
            fail(error);
        s.message = "Collection created successfully";
    } else if (command == "dropCollection" && !collName.empty()) {
        auto coll = client.collection(dbName, collName);
        if (!dropCollection(coll.get(), &error))
            fail(error);
        s.message = "Collection dropped successfully";
    } else if (command == "runCommand") {
        bson_t commandDoc;
        if (findDocument(cmd, "commandDoc", &commandDoc)) {
            auto db = client.database(dbName);
            Bson reply;
            if (!mongoc_database_command_with_opts(db.get(), &commandDoc, nullptr, nullptr,
                                                   reply.get(), &error))
                fail(error);
            s.columnNames.push_back("result");
            s.tableData.push_back({toJson(reply.get())});
            s.message = "Command executed successfully";
        }
    } else {
        s.success = false;
        s.errorMessage = "Unknown command or missing collection name";
    }
}

std::vector<std::vector<std::string>>
MongoDBDatabase::getTableData(const Table& table, int limit, int offset,
                              const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;
    if (!pool_)
        return result;
    const auto columnNames = getColumnNames(table);

    Client client(pool_);
    auto coll = client.collection(name_, table.name);

    Bson opts, empty;
    BSON_APPEND_INT64(opts.get(), "limit", limit);
    BSON_APPEND_INT64(opts.get(), "skip", offset);

    // an invalid filter or sort falls back to none
    bson_error_t error;
    BsonPtr filterDoc;
    if (!whereClause.empty())
        filterDoc = fromJson(whereClause, &error);
    if (!orderByClause.empty())
        if (auto sort = fromJson(orderByClause, &error))
            BSON_APPEND_DOCUMENT(opts.get(), "sort", sort.get());

    CursorHandle cursor(mongoc_collection_find_with_opts(
        coll.get(), filterDoc ? filterDoc.get() : empty.get(), opts.get(), nullptr));
    forEach(cursor.get(), [&](const bson_t* doc) {
        std::vector<std::string> row;
        row.reserve(columnNames.size());
        for (const auto& colName : columnNames) {
            bson_iter_t it;
            if (bson_iter_init_find(&it, doc, colName.c_str()))
                row.push_back(elementToString(&it));
            else
                row.push_back("");
        }
        result.push_back(std::move(row));
    });
    return result;
}

std::vector<std::string> MongoDBDatabase::getColumnNames(const Table& table) {
    if (!table.columns.empty()) {
        std::vector<std::string> names;
        names.reserve(table.columns.size());
        for (const auto& col : table.columns)
            names.push_back(col.name);
        return names;
    }
    if (!pool_)
        return {"_id", "document"};
    Client client(pool_);
    auto cols = inferSchemaFromSample(client, table.name, 100);
    if (!cols.empty()) {
        std::vector<std::string> names;
        names.reserve(cols.size());
        for (const auto& col : cols)
            names.push_back(col.name);
        return names;
    }
    return {"_id", "document"};
}

int MongoDBDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    if (!pool_)
        return 0;
    Client client(pool_);
    auto coll = client.collection(name_, table.name);
    bson_error_t error;
    BsonPtr filterDoc;
    if (!whereClause.empty())
        filterDoc = fromJson(whereClause, &error);
    Bson empty;
    const int64_t n = mongoc_collection_count_documents(
        coll.get(), filterDoc ? filterDoc.get() : empty.get(), nullptr, nullptr, nullptr, &error);
    if (n < 0)
        fail(error);
    return static_cast<int>(n);
}

Status MongoDBDatabase::createTable(const Table& table) {
    if (!pool_)
        return {false, "Not connected"};
    Client client(pool_);
    auto db = client.database(name_);
    bson_error_t error;
    CollectionHandle created(
        mongoc_database_create_collection(db.get(), table.name.c_str(), nullptr, &error));
    if (!created)
        return {false, error.message};
    return {true, ""};
}

Status MongoDBDatabase::dropTable(const std::string& tableName) {
    if (!pool_)
        return {false, "Not connected"};
    Client client(pool_);
    auto coll = client.collection(name_, tableName);
    bson_error_t error;
    if (!dropCollection(coll.get(), &error))
        return {false, error.message};
    return {true, ""};
}

// Connection impl wraps the pool + per-database cache.
class ConnectionImpl {
public:
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {
        if (info_.port == 0 || info_.port == 5432)
            info_.port = 27017;
    }

    Status open() {
        initDriver();
        const std::string uriString = info_.buildConnectionString();
        bson_error_t error;
        std::unique_ptr<mongoc_uri_t, decltype(&mongoc_uri_destroy)> uri(
            mongoc_uri_new_with_error(uriString.c_str(), &error), &mongoc_uri_destroy);
        if (!uri)
            return failOpen(error.message);
        // the pool reads tls options (tlsCAFile, ...) from the uri
        PoolPtr pool(mongoc_client_pool_new_with_error(uri.get(), &error),
                     &mongoc_client_pool_destroy);
        if (!pool)
            return failOpen(error.message);
        mongoc_client_pool_set_error_api(pool.get(), MONGOC_ERROR_API_VERSION_2);
        try {
            // smoke test: list databases
            Client(pool).databaseNames();
        } catch (const std::exception& e) {
            return failOpen(e.what());
        }
        std::lock_guard lock(mu_);
        pool_ = std::move(pool);
        open_ = true;
        return {true, ""};
    }

    void close() {
        std::lock_guard lock(mu_);
        cache_.clear();
        pool_.reset();
        open_ = false;
    }

    bool isOpen() const {
        return open_;
    }
    const ConnectionInfo& info() const {
        return info_;
    }

    std::vector<DatabasePtr> databases() {
        std::vector<DatabasePtr> out;
        PoolPtr pool;
        {
            std::lock_guard lock(mu_);
            pool = pool_;
        }
        if (!pool)
            return out;
        for (const auto& n : Client(pool).databaseNames())
            out.push_back(database(n));
        return out;
    }

    DatabasePtr database(const std::string& name) {
        std::string n = name.empty() ? info_.database : name;
        if (n.empty())
            n = "test";
        std::lock_guard lock(mu_);
        if (!pool_)
            return nullptr;
        if (auto it = cache_.find(n); it != cache_.end())
            return it->second;
        auto db = std::make_shared<MongoDBDatabase>(pool_, info_.name, n);
        cache_[n] = db;
        return db;
    }

    Status dropDatabase(const std::string& name) {
        PoolPtr pool;
        {
            std::lock_guard lock(mu_);
            cache_.erase(name);
            pool = pool_;
        }
        if (!pool)
            return {false, "Not connected"};
        Client client(pool);
        auto db = client.database(name);
        bson_error_t error;
        if (!mongoc_database_drop(db.get(), &error))
            return {false, error.message};
        return {true, ""};
    }

private:
    Status failOpen(const std::string& message) {
        std::lock_guard lock(mu_);
        pool_.reset();
        open_ = false;
        return {false, "MongoDB connection failed: " + message};
    }

    ConnectionInfo info_;
    PoolPtr pool_;
    std::unordered_map<std::string, std::shared_ptr<MongoDBDatabase>> cache_;
    bool open_ = false;
    mutable std::mutex mu_;
};

} // namespace

MongoDBConnection::MongoDBConnection(const ConnectionInfo& info) : info_(info) {
    impl_ = new ConnectionImpl(info);
}
MongoDBConnection::~MongoDBConnection() {
    delete static_cast<ConnectionImpl*>(impl_);
}
Status MongoDBConnection::open() {
    return static_cast<ConnectionImpl*>(impl_)->open();
}
void MongoDBConnection::close() {
    static_cast<ConnectionImpl*>(impl_)->close();
}
bool MongoDBConnection::isOpen() const {
    return static_cast<const ConnectionImpl*>(impl_)->isOpen();
}
std::vector<DatabasePtr> MongoDBConnection::databases() {
    return static_cast<ConnectionImpl*>(impl_)->databases();
}
DatabasePtr MongoDBConnection::database(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->database(name);
}
Status MongoDBConnection::dropDatabase(const std::string& name) {
    return static_cast<ConnectionImpl*>(impl_)->dropDatabase(name);
}

} // namespace dearsql
