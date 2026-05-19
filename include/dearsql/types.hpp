#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dearsql {

// sentinel prefix to distinguish real SQL NULL from the literal string "NULL"
inline constexpr std::string_view NULL_SENTINEL = "dearsql__null";

inline bool isNullSentinel(const std::string& v) {
    return v == NULL_SENTINEL;
}

// sentinels for boolean values
inline constexpr std::string_view BOOL_TRUE_SENTINEL = "dearsql__bool_true";
inline constexpr std::string_view BOOL_FALSE_SENTINEL = "dearsql__bool_false";

inline bool isBoolSentinel(const std::string& v) {
    return v == BOOL_TRUE_SENTINEL || v == BOOL_FALSE_SENTINEL;
}

inline bool boolSentinelValue(const std::string& v) {
    return v == BOOL_TRUE_SENTINEL;
}

struct Column {
    std::string name;
    std::string type;
    std::string defaultValue;
    std::string comment;
    bool isPrimaryKey = false;
    bool isNotNull = false;
    bool isUnique = false;
    bool isAutoIncrement = false;
};

struct Index {
    std::string name;
    std::vector<std::string> columns;
    bool isUnique = false;
    bool isPrimary = false;
    std::string type; // BTREE, HASH, etc.
};

struct ForeignKey {
    std::string name;
    std::string sourceColumn;
    std::string targetTable;
    std::string targetColumn;
    std::string onDelete;
    std::string onUpdate;
};

struct Table {
    std::string name;       // simple name, e.g. "users"
    std::string schema;     // schema/namespace for SQL qualification (empty for SQLite/MySQL-DB-only)
    std::string comment;
    std::string definition; // view body (SELECT after `AS`); empty for tables
    std::string fullName;   // fully qualified, e.g. "conn.db.schema.table"

    std::vector<Column> columns;
    std::vector<Index> indexes;
    std::vector<ForeignKey> foreignKeys;
    std::vector<ForeignKey> incomingForeignKeys;
    std::unordered_map<std::string, ForeignKey> foreignKeysByColumn;

    int64_t sizeBytes = -1; // -1 if unknown
};

enum class RoutineKind { Function, Procedure };

struct Routine {
    std::string name;
    std::string signature; // e.g. "my_func(integer, text)"
    RoutineKind kind = RoutineKind::Function;
    std::string returnType;
};

// build foreignKeysByColumn lookup from foreignKeys
void buildForeignKeyLookup(Table& table);

// populate incomingForeignKeys on each table based on cross-references
void populateIncomingForeignKeys(std::vector<Table>& tables);

std::string formatByteSize(int64_t bytes);

} // namespace dearsql
