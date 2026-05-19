#include "dearsql/backends/mongodb_connection.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/uri.hpp>

namespace dearsql {

namespace {

// process-wide singleton; mongocxx requires exactly one instance.
mongocxx::instance& getDriverInstance() {
    static mongocxx::instance instance{};
    return instance;
}

std::string bsonElementToString(const bsoncxx::document::element& elem) {
    if (!elem)
        return "";
    switch (elem.type()) {
    case bsoncxx::type::k_string:
        return std::string(elem.get_string().value);
    case bsoncxx::type::k_int32:
        return std::to_string(elem.get_int32().value);
    case bsoncxx::type::k_int64:
        return std::to_string(elem.get_int64().value);
    case bsoncxx::type::k_double:
        return std::to_string(elem.get_double().value);
    case bsoncxx::type::k_bool:
        return elem.get_bool().value ? std::string(BOOL_TRUE_SENTINEL)
                                     : std::string(BOOL_FALSE_SENTINEL);
    case bsoncxx::type::k_oid:
        return elem.get_oid().value.to_string();
    case bsoncxx::type::k_date: {
        const auto millis = elem.get_date().value.count();
        const auto seconds = millis / 1000;
        const auto time = static_cast<std::time_t>(seconds);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return std::string(buf);
    }
    case bsoncxx::type::k_null:
        return std::string(NULL_SENTINEL);
    case bsoncxx::type::k_decimal128:
        return elem.get_decimal128().value.to_string();
    case bsoncxx::type::k_document:
        return bsoncxx::to_json(elem.get_document().value);
    case bsoncxx::type::k_array:
        return bsoncxx::to_json(elem.get_array().value);
    default:
        try {
            auto wrapper = bsoncxx::builder::stream::document{}
                           << "v" << elem.get_value() << bsoncxx::builder::stream::finalize;
            return bsoncxx::to_json(wrapper.view());
        } catch (...) {
            return "<unknown>";
        }
    }
}

// Per-database handle. Holds a shared_ptr to the pool so the pool outlives any
// database handle handed out by the connection.
class MongoDBDatabase final : public IDatabase {
public:
    MongoDBDatabase(std::shared_ptr<mongocxx::pool> pool, std::string connName, std::string dbName)
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
    std::vector<Column> inferSchemaFromSample(mongocxx::pool::entry& client,
                                              const std::string& collectionName, int sampleSize);
    std::vector<Index> getCollectionIndexes(mongocxx::pool::entry& client,
                                            const std::string& collectionName);

    std::shared_ptr<mongocxx::pool> pool_;
    std::string connName_;
    std::string name_;
};

std::vector<Table> MongoDBDatabase::tables() {
    std::vector<Table> result;
    if (!pool_)
        return result;
    try {
        auto client = pool_->acquire();
        auto db = (*client)[name_];
        auto names = db.list_collection_names();
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
    } catch (...) {
    }
    return result;
}

Table MongoDBDatabase::describeTable(const std::string& tableName) {
    Table t;
    t.name = tableName;
    t.fullName = connName_ + "." + name_ + "." + tableName;
    if (!pool_)
        return t;
    try {
        auto client = pool_->acquire();
        t.columns = inferSchemaFromSample(client, tableName, 100);
        t.indexes = getCollectionIndexes(client, tableName);
    } catch (...) {
    }
    return t;
}

std::vector<Column> MongoDBDatabase::inferSchemaFromSample(mongocxx::pool::entry& client,
                                                           const std::string& collectionName,
                                                           int sampleSize) {
    std::vector<Column> columns;
    std::set<std::string> seenFields;
    try {
        auto db = (*client)[name_];
        auto coll = db[collectionName];
        mongocxx::options::find opts;
        opts.limit(sampleSize);
        auto cursor = coll.find({}, opts);
        for (auto&& doc : cursor) {
            for (auto&& elem : doc) {
                std::string fieldName(elem.key());
                if (seenFields.contains(fieldName))
                    continue;
                seenFields.insert(fieldName);

                Column col;
                col.name = fieldName;
                switch (elem.type()) {
                case bsoncxx::type::k_double:
                    col.type = "double";
                    break;
                case bsoncxx::type::k_string:
                    col.type = "string";
                    break;
                case bsoncxx::type::k_document:
                    col.type = "object";
                    break;
                case bsoncxx::type::k_array:
                    col.type = "array";
                    break;
                case bsoncxx::type::k_binary:
                    col.type = "binary";
                    break;
                case bsoncxx::type::k_oid:
                    col.type = "objectId";
                    break;
                case bsoncxx::type::k_bool:
                    col.type = "bool";
                    break;
                case bsoncxx::type::k_date:
                    col.type = "date";
                    break;
                case bsoncxx::type::k_null:
                    col.type = "null";
                    break;
                case bsoncxx::type::k_int32:
                    col.type = "int32";
                    break;
                case bsoncxx::type::k_int64:
                    col.type = "int64";
                    break;
                case bsoncxx::type::k_decimal128:
                    col.type = "decimal128";
                    break;
                default:
                    col.type = "unknown";
                    break;
                }
                if (fieldName == "_id") {
                    col.isPrimaryKey = true;
                    col.isNotNull = true;
                }
                columns.push_back(std::move(col));
            }
        }
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

std::vector<Index> MongoDBDatabase::getCollectionIndexes(mongocxx::pool::entry& client,
                                                         const std::string& collectionName) {
    std::vector<Index> indexes;
    try {
        auto db = (*client)[name_];
        auto coll = db[collectionName];
        auto cursor = coll.list_indexes();
        for (auto&& doc : cursor) {
            Index idx;
            if (auto it = doc.find("name"); it != doc.end()) {
                idx.name = std::string(it->get_string().value);
            }
            if (auto it = doc.find("key");
                it != doc.end() && it->type() == bsoncxx::type::k_document) {
                for (auto&& field : it->get_document().value)
                    idx.columns.emplace_back(field.key());
            }
            if (auto it = doc.find("unique");
                it != doc.end() && it->type() == bsoncxx::type::k_bool) {
                idx.isUnique = it->get_bool().value;
            }
            if (idx.name == "_id_") {
                idx.isPrimary = true;
                idx.isUnique = true;
            }
            indexes.push_back(std::move(idx));
        }
    } catch (...) {
    }
    return indexes;
}

// Execute a JSON command string. Two protocols are accepted:
//   1) Wrapper form: {"database":"...","collection":"...","command":"find",...}
//   2) Native MongoDB command (passed straight to run_command), e.g.
//      {"insert":"coll","documents":[{...}]} or {"find":"coll","filter":{...}}.
// Detection: if the parsed doc has a top-level "command" string, use form (1);
// otherwise treat it as a native runCommand body.
QueryResult MongoDBDatabase::execute(const std::string& sql, int rowLimit) {
    QueryResult result;
    StatementResult s;
    const auto startTime = std::chrono::high_resolution_clock::now();
    if (!pool_) {
        s.success = false;
        s.errorMessage = "Not connected";
        result.statements.push_back(std::move(s));
        return result;
    }

    bsoncxx::document::value doc(bsoncxx::builder::stream::document{}
                                 << bsoncxx::builder::stream::finalize);
    try {
        doc = bsoncxx::from_json(sql);
    } catch (const std::exception& e) {
        s.success = false;
        s.errorMessage = std::string("Invalid JSON command: ") + e.what();
        result.statements.push_back(std::move(s));
        const auto endTime = std::chrono::high_resolution_clock::now();
        result.executionTimeMs =
            std::chrono::duration<double, std::milli>(endTime - startTime).count();
        return result;
    }

    try {
        auto view = doc.view();
        auto client = pool_->acquire();
        auto db = (*client)[name_];

        // Detect wrapper form by presence of a string "command" element.
        const bool wrapperForm = view["command"] && view["command"].type() == bsoncxx::type::k_string;

        if (wrapperForm) {
            std::string dbName = name_;
            std::string collName;
            std::string command = "find";
            if (view["database"] && view["database"].type() == bsoncxx::type::k_string)
                dbName = std::string(view["database"].get_string().value);
            if (view["collection"] && view["collection"].type() == bsoncxx::type::k_string)
                collName = std::string(view["collection"].get_string().value);
            command = std::string(view["command"].get_string().value);

            auto targetDb = (*client)[dbName];

            if (command == "find" && !collName.empty()) {
                auto coll = targetDb[collName];
                bsoncxx::document::view_or_value filter = bsoncxx::builder::stream::document{}
                                                          << bsoncxx::builder::stream::finalize;
                if (view["filter"])
                    filter = view["filter"].get_document().value;
                mongocxx::options::find opts;
                opts.limit(rowLimit);
                auto cursor = coll.find(filter, opts);
                s.columnNames.push_back("_id");
                s.columnNames.push_back("document");
                for (auto&& d : cursor) {
                    std::vector<std::string> row;
                    if (d["_id"]) {
                        auto idDoc = bsoncxx::builder::stream::document{}
                                     << "_id" << d["_id"].get_value()
                                     << bsoncxx::builder::stream::finalize;
                        row.push_back(bsoncxx::to_json(idDoc.view()));
                    } else {
                        row.push_back("");
                    }
                    row.push_back(bsoncxx::to_json(d));
                    s.tableData.push_back(std::move(row));
                }
                s.message = std::format("Returned {} document{}", s.tableData.size(),
                                        s.tableData.size() == 1 ? "" : "s");
            } else if (command == "aggregate" && !collName.empty()) {
                auto coll = targetDb[collName];
                mongocxx::pipeline pipeline;
                if (view["pipeline"]) {
                    for (auto&& stage : view["pipeline"].get_array().value)
                        pipeline.append_stage(stage.get_document().value);
                }
                auto cursor = coll.aggregate(pipeline);
                s.columnNames.push_back("document");
                for (auto&& d : cursor) {
                    std::vector<std::string> row;
                    row.push_back(bsoncxx::to_json(d));
                    s.tableData.push_back(std::move(row));
                }
                s.message = std::format("Returned {} document{}", s.tableData.size(),
                                        s.tableData.size() == 1 ? "" : "s");
            } else if (command == "insert" && !collName.empty()) {
                auto coll = targetDb[collName];
                if (view["document"]) {
                    coll.insert_one(view["document"].get_document().value);
                    s.affectedRows = 1;
                } else if (view["documents"]) {
                    std::vector<bsoncxx::document::view> docs;
                    for (auto&& d : view["documents"].get_array().value)
                        docs.push_back(d.get_document().value);
                    auto r = coll.insert_many(docs);
                    s.affectedRows = r ? static_cast<int>(r->inserted_count()) : 0;
                }
                s.message = "Insert executed successfully";
            } else if (command == "update" && !collName.empty()) {
                auto coll = targetDb[collName];
                auto filter = view["filter"].get_document().value;
                auto update = view["update"].get_document().value;
                auto r = coll.update_many(filter, update);
                s.affectedRows = r ? static_cast<int>(r->modified_count()) : 0;
                s.message = std::format("Updated {} document{}", s.affectedRows,
                                        s.affectedRows == 1 ? "" : "s");
            } else if (command == "delete" && !collName.empty()) {
                auto coll = targetDb[collName];
                auto filter = view["filter"].get_document().value;
                auto r = coll.delete_many(filter);
                s.affectedRows = r ? static_cast<int>(r->deleted_count()) : 0;
                s.message = std::format("Deleted {} document{}", s.affectedRows,
                                        s.affectedRows == 1 ? "" : "s");
            } else if (command == "createCollection" && !collName.empty()) {
                targetDb.create_collection(collName);
                s.message = "Collection created successfully";
            } else if (command == "dropCollection" && !collName.empty()) {
                targetDb[collName].drop();
                s.message = "Collection dropped successfully";
            } else if (command == "runCommand") {
                if (view["commandDoc"]) {
                    auto cmdResult = targetDb.run_command(view["commandDoc"].get_document().value);
                    s.columnNames.push_back("result");
                    std::vector<std::string> row;
                    row.push_back(bsoncxx::to_json(cmdResult.view()));
                    s.tableData.push_back(std::move(row));
                    s.message = "Command executed successfully";
                }
            } else {
                s.success = false;
                s.errorMessage = "Unknown command or missing collection name";
            }
        } else {
            // Native runCommand: pass the document straight to the server.
            auto cmdResult = db.run_command(view);
            s.columnNames.push_back("result");
            std::vector<std::string> row;
            row.push_back(bsoncxx::to_json(cmdResult.view()));
            s.tableData.push_back(std::move(row));
            s.message = "Command executed successfully";
        }
    } catch (const std::exception& e) {
        s.success = false;
        s.errorMessage = e.what();
    }

    const auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTimeMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();
    result.statements.push_back(std::move(s));
    return result;
}

std::vector<std::vector<std::string>>
MongoDBDatabase::getTableData(const Table& table, int limit, int offset,
                              const std::string& whereClause, const std::string& orderByClause) {
    std::vector<std::vector<std::string>> result;
    if (!pool_)
        return result;
    try {
        auto client = pool_->acquire();
        auto db = (*client)[name_];
        auto coll = db[table.name];

        auto columnNames = getColumnNames(table);

        mongocxx::options::find opts;
        opts.limit(limit);
        opts.skip(offset);

        bsoncxx::document::view_or_value filterDoc = bsoncxx::builder::stream::document{}
                                                     << bsoncxx::builder::stream::finalize;
        if (!whereClause.empty()) {
            try {
                filterDoc = bsoncxx::from_json(whereClause);
            } catch (...) {
                // invalid filter, fall back to empty
            }
        }
        if (!orderByClause.empty()) {
            try {
                opts.sort(bsoncxx::from_json(orderByClause));
            } catch (...) {
            }
        }

        auto cursor = coll.find(filterDoc, opts);
        for (auto&& doc : cursor) {
            std::vector<std::string> row;
            row.reserve(columnNames.size());
            for (const auto& colName : columnNames) {
                if (auto elem = doc[colName])
                    row.push_back(bsonElementToString(elem));
                else
                    row.push_back("");
            }
            result.push_back(std::move(row));
        }
    } catch (...) {
    }
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
    try {
        auto client = pool_->acquire();
        auto cols = inferSchemaFromSample(client, table.name, 100);
        if (!cols.empty()) {
            std::vector<std::string> names;
            names.reserve(cols.size());
            for (const auto& col : cols)
                names.push_back(col.name);
            return names;
        }
    } catch (...) {
    }
    return {"_id", "document"};
}

int MongoDBDatabase::getRowCount(const Table& table, const std::string& whereClause) {
    if (!pool_)
        return 0;
    try {
        auto client = pool_->acquire();
        auto db = (*client)[name_];
        auto coll = db[table.name];
        bsoncxx::document::view_or_value filterDoc = bsoncxx::builder::stream::document{}
                                                     << bsoncxx::builder::stream::finalize;
        if (!whereClause.empty()) {
            try {
                filterDoc = bsoncxx::from_json(whereClause);
            } catch (...) {
            }
        }
        return static_cast<int>(coll.count_documents(filterDoc));
    } catch (...) {
        return 0;
    }
}

Status MongoDBDatabase::createTable(const Table& table) {
    if (!pool_)
        return {false, "Not connected"};
    try {
        auto client = pool_->acquire();
        auto db = (*client)[name_];
        db.create_collection(table.name);
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

Status MongoDBDatabase::dropTable(const std::string& tableName) {
    if (!pool_)
        return {false, "Not connected"};
    try {
        auto client = pool_->acquire();
        auto db = (*client)[name_];
        db[tableName].drop();
        return {true, ""};
    } catch (const std::exception& e) {
        return {false, e.what()};
    }
}

// Connection impl wraps the pool + per-database cache.
class ConnectionImpl {
public:
    explicit ConnectionImpl(ConnectionInfo info) : info_(std::move(info)) {
        if (info_.port == 0 || info_.port == 5432)
            info_.port = 27017;
    }

    Status open() {
        getDriverInstance();
        try {
            std::string uri = info_.buildConnectionString();
            std::lock_guard lock(mu_);
            pool_ = std::make_shared<mongocxx::pool>(mongocxx::uri{uri});
            // smoke test: acquire a client and list databases
            auto client = pool_->acquire();
            (void)client->list_database_names();
            open_ = true;
            return {true, ""};
        } catch (const std::exception& e) {
            std::lock_guard lock(mu_);
            pool_.reset();
            open_ = false;
            return {false, std::string("MongoDB connection failed: ") + e.what()};
        }
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
        std::shared_ptr<mongocxx::pool> pool;
        {
            std::lock_guard lock(mu_);
            pool = pool_;
        }
        if (!pool)
            return out;
        try {
            auto client = pool->acquire();
            auto names = client->list_database_names();
            for (const auto& n : names)
                out.push_back(database(n));
        } catch (...) {
        }
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
        std::shared_ptr<mongocxx::pool> pool;
        {
            std::lock_guard lock(mu_);
            cache_.erase(name);
            pool = pool_;
        }
        if (!pool)
            return {false, "Not connected"};
        try {
            auto client = pool->acquire();
            (*client)[name].drop();
            return {true, ""};
        } catch (const std::exception& e) {
            return {false, e.what()};
        }
    }

private:
    ConnectionInfo info_;
    std::shared_ptr<mongocxx::pool> pool_;
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
