#include "dearsql/completion.hpp"
#include "dearsql/sql_builder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <initializer_list>
#include <memory>
#include <unordered_set>

namespace dearsql {

namespace {

// ---------- string helpers ----------

char lowerc(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

char upperc(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = lowerc(c);
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out)
        c = upperc(c);
    return out;
}

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lowerc(a[i]) != lowerc(b[i]))
            return false;
    }
    return true;
}

bool istartsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && ieq(s.substr(0, p.size()), p);
}

size_t ifind(std::string_view s, std::string_view p, size_t from = 0) {
    if (p.empty())
        return from;
    for (size_t i = from; i + p.size() <= s.size(); ++i) {
        if (ieq(s.substr(i, p.size()), p))
            return i;
    }
    return std::string_view::npos;
}

bool isSubsequence(std::string_view s, std::string_view p) {
    size_t j = 0;
    for (size_t i = 0; i < s.size() && j < p.size(); ++i) {
        if (lowerc(s[i]) == lowerc(p[j]))
            ++j;
    }
    return j == p.size();
}

bool iless(std::string_view a, std::string_view b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                        [](char x, char y) { return lowerc(x) < lowerc(y); });
}

std::vector<std::string> splitPath(std::string_view path) {
    std::vector<std::string> parts;
    if (path.empty())
        return parts;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        parts.emplace_back(path.substr(start, dot == std::string_view::npos ? std::string_view::npos
                                                                            : dot - start));
        if (dot == std::string_view::npos)
            break;
        start = dot + 1;
    }
    return parts;
}

bool isIdentStartByte(unsigned char c) {
    return std::isalpha(c) || c == '_' || c >= 0x80;
}

bool isIdentByte(unsigned char c) {
    return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

using WordSet = std::unordered_set<std::string_view>;

// ---------- dialect ----------

struct Dialect {
    bool backtickIdent = false;
    bool bracketIdent = false;
    bool doubleQuoteIdent = true;
    bool dollarQuote = false;
    bool hashComment = false;
    bool backslashEscape = false;
    bool hashIdent = false; // mssql #temp tables
};

Dialect dialectFor(DatabaseType type) {
    Dialect d;
    switch (type) {
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        d.backtickIdent = true;
        d.doubleQuoteIdent = false;
        d.hashComment = true;
        d.backslashEscape = true;
        break;
    case DatabaseType::SQLITE:
        d.backtickIdent = true;
        d.bracketIdent = true;
        break;
    case DatabaseType::MSSQL:
        d.bracketIdent = true;
        d.hashIdent = true;
        break;
    case DatabaseType::POSTGRESQL:
    case DatabaseType::REDSHIFT:
    case DatabaseType::DUCKDB:
    case DatabaseType::CASSANDRA:
        d.dollarQuote = true;
        break;
    default:
        break;
    }
    return d;
}

bool isSqlDialect(DatabaseType type) {
    return type != DatabaseType::REDIS && type != DatabaseType::MONGODB;
}

// ---------- tokenizer ----------

enum class Tok : uint8_t { Word, QuotedIdent, String, Number, Comment, Param, Punct, Op };

struct Token {
    Tok kind = Tok::Op;
    size_t begin = 0;
    size_t end = 0;
    bool closed = true;       // strings, comments, quoted identifiers
    bool lineComment = false; // runs to the end of the line
    std::string text;         // identifier text, unquoted; punct/op char
};

std::vector<Token> tokenize(std::string_view s, const Dialect& d) {
    std::vector<Token> out;
    const size_t n = s.size();
    size_t i = 0;

    auto scanQuoted = [&](Token& t, char close, bool backslash) {
        size_t j = t.begin + 1;
        t.closed = false;
        while (j < n) {
            const char ch = s[j];
            if (backslash && ch == '\\' && j + 1 < n) {
                t.text += s[j + 1];
                j += 2;
                continue;
            }
            if (ch == close) {
                if (j + 1 < n && s[j + 1] == close) {
                    t.text += close;
                    j += 2;
                    continue;
                }
                t.closed = true;
                ++j;
                break;
            }
            t.text += ch;
            ++j;
        }
        t.end = j;
    };

    while (i < n) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (std::isspace(c)) {
            ++i;
            continue;
        }
        Token t;
        t.begin = i;
        const char next = i + 1 < n ? s[i + 1] : '\0';

        if ((c == '-' && next == '-') || (c == '#' && d.hashComment)) {
            size_t e = s.find('\n', i);
            if (e == std::string_view::npos)
                e = n;
            t.kind = Tok::Comment;
            t.lineComment = true;
            t.end = e;
        } else if (c == '/' && next == '*') {
            const size_t e = s.find("*/", i + 2);
            t.kind = Tok::Comment;
            t.closed = e != std::string_view::npos;
            t.end = t.closed ? e + 2 : n;
        } else if (c == '\'') {
            t.kind = Tok::String;
            scanQuoted(t, '\'', d.backslashEscape);
        } else if (c == '"') {
            t.kind = d.doubleQuoteIdent ? Tok::QuotedIdent : Tok::String;
            scanQuoted(t, '"', !d.doubleQuoteIdent && d.backslashEscape);
        } else if (c == '`' && d.backtickIdent) {
            t.kind = Tok::QuotedIdent;
            scanQuoted(t, '`', false);
        } else if (c == '[' && d.bracketIdent) {
            t.kind = Tok::QuotedIdent;
            scanQuoted(t, ']', false);
        } else if (c == '$' && d.dollarQuote &&
                   (next == '$' || isIdentStartByte(static_cast<unsigned char>(next)))) {
            size_t k = i + 1;
            while (k < n && s[k] != '$' && isIdentByte(static_cast<unsigned char>(s[k])))
                ++k;
            if (k < n && s[k] == '$') {
                const std::string_view tag = s.substr(i, k - i + 1);
                const size_t e = s.find(tag, k + 1);
                t.kind = Tok::String;
                t.closed = e != std::string_view::npos;
                t.end = t.closed ? e + tag.size() : n;
            } else {
                t.kind = Tok::Op;
                t.text = "$";
                t.end = i + 1;
            }
        } else if (std::isdigit(c) ||
                   (c == '.' && std::isdigit(static_cast<unsigned char>(next)) &&
                    (i == 0 ||
                     (!isIdentByte(static_cast<unsigned char>(s[i - 1])) && s[i - 1] != '"' &&
                      s[i - 1] != '`' && s[i - 1] != ']' && s[i - 1] != ')')))) {
            size_t k = i + 1;
            while (k < n) {
                const auto ch = static_cast<unsigned char>(s[k]);
                if (std::isalnum(ch) || ch == '_' || ch == '.') {
                    ++k;
                } else if ((ch == '+' || ch == '-') && (s[k - 1] == 'e' || s[k - 1] == 'E') &&
                           k + 1 < n && std::isdigit(static_cast<unsigned char>(s[k + 1]))) {
                    ++k;
                } else {
                    break;
                }
            }
            t.kind = Tok::Number;
            t.end = k;
        } else if (isIdentStartByte(c) || (c == '#' && d.hashIdent)) {
            size_t k = i + 1;
            while (k < n &&
                   (isIdentByte(static_cast<unsigned char>(s[k])) || (d.hashIdent && s[k] == '#')))
                ++k;
            t.kind = Tok::Word;
            t.end = k;
            t.text = std::string(s.substr(i, k - i));
        } else if (c == '@' || c == '?' ||
                   (c == ':' && isIdentStartByte(static_cast<unsigned char>(next)) &&
                    (i == 0 || s[i - 1] != ':')) ||
                   (c == '$' && std::isdigit(static_cast<unsigned char>(next)))) {
            size_t k = i + 1;
            while (k < n && (s[k] == '@' || isIdentByte(static_cast<unsigned char>(s[k]))))
                ++k;
            t.kind = Tok::Param;
            t.end = k;
        } else if (c == '(' || c == ')' || c == ',' || c == ';' || c == '.') {
            t.kind = Tok::Punct;
            t.text = std::string(1, static_cast<char>(c));
            t.end = i + 1;
        } else {
            t.kind = Tok::Op;
            t.text = std::string(1, static_cast<char>(c));
            t.end = i + 1;
        }
        i = std::max(t.end, i + 1);
        out.push_back(std::move(t));
    }
    return out;
}

// statement bounds around `offset`: [begin, end) in bytes, plus the token
// index range of the statement in `all`
struct StatementSpan {
    size_t begin = 0;
    size_t end = 0;
    size_t firstTok = 0;
    size_t lastTok = 0; // one past
};

StatementSpan statementAt(const std::vector<Token>& all, size_t offset, size_t length) {
    StatementSpan span{0, length, 0, all.size()};
    for (size_t i = 0; i < all.size(); ++i) {
        const auto& t = all[i];
        if (t.kind != Tok::Punct || t.text != ";")
            continue;
        if (t.end <= offset) {
            span.begin = t.end;
            span.firstTok = i + 1;
        } else {
            span.end = t.begin;
            span.lastTok = i;
            break;
        }
    }
    return span;
}

// ---------- keyword and function tables ----------

constexpr size_t kTypeCount = static_cast<size_t>(DatabaseType::DUCKDB) + 1;

// builders are stateless; one per dialect, shared
const ISQLBuilder& builderFor(DatabaseType type) {
    static const auto all = [] {
        std::array<std::unique_ptr<ISQLBuilder>, kTypeCount> a;
        for (size_t i = 0; i < kTypeCount; ++i)
            a[i] = createSQLBuilder(static_cast<DatabaseType>(i));
        return a;
    }();
    return *all[std::min(static_cast<size_t>(type), kTypeCount - 1)];
}

void append(std::vector<std::string>& out, std::initializer_list<const char*> words) {
    for (const char* w : words)
        out.emplace_back(w);
}

void sortUnique(std::vector<std::string>& v) {
    std::ranges::sort(v);
    auto dup = std::ranges::unique(v);
    v.erase(dup.begin(), dup.end());
}

std::vector<std::string> buildKeywords(DatabaseType type) {
    std::vector<std::string> kw;
    if (!isSqlDialect(type))
        return kw;
    if (type == DatabaseType::CASSANDRA) {
        append(kw,
               {"SELECT",      "FROM",     "WHERE",      "AND",       "IN",           "INSERT",
                "INTO",        "VALUES",   "UPDATE",     "SET",       "DELETE",       "CREATE",
                "DROP",        "ALTER",    "TRUNCATE",   "USE",       "KEYSPACE",     "KEYSPACES",
                "TABLE",       "TABLES",   "INDEX",      "TYPE",      "MATERIALIZED", "VIEW",
                "PRIMARY",     "KEY",      "CLUSTERING", "ORDER",     "BY",           "ASC",
                "DESC",        "LIMIT",    "PER",        "PARTITION", "ALLOW",        "FILTERING",
                "IF",          "EXISTS",   "NOT",        "USING",     "TTL",          "TIMESTAMP",
                "BATCH",       "APPLY",    "BEGIN",      "UNLOGGED",  "COUNTER",      "WITH",
                "REPLICATION", "CONTAINS", "DISTINCT",   "JSON",      "AS",           "NULL",
                "TRUE",        "FALSE",    "ADD",        "RENAME",    "TO",           "GRANT",
                "REVOKE",      "ROLE",     "DESCRIBE",   "TEXT",      "INT",          "BIGINT",
                "SMALLINT",    "TINYINT",  "UUID",       "TIMEUUID",  "BOOLEAN",      "FLOAT",
                "DOUBLE",      "DECIMAL",  "VARINT",     "LIST",      "MAP",          "FROZEN",
                "TUPLE",       "BLOB",     "DATE",       "TIME",      "INET",         "ASCII",
                "VARCHAR",     "DURATION", "STATIC"});
        sortUnique(kw);
        return kw;
    }

    append(
        kw,
        {"SELECT",      "FROM",       "WHERE",   "AND",       "OR",       "NOT",       "IN",
         "IS",          "NULL",       "AS",      "ON",        "JOIN",     "LEFT",      "RIGHT",
         "INNER",       "OUTER",      "CROSS",   "FULL",      "NATURAL",  "GROUP",     "BY",
         "ORDER",       "ASC",        "DESC",    "HAVING",    "UNION",    "ALL",       "DISTINCT",
         "BETWEEN",     "LIKE",       "EXISTS",  "CASE",      "WHEN",     "THEN",      "ELSE",
         "END",         "BEGIN",      "COMMIT",  "ROLLBACK",  "WITH",     "RECURSIVE", "CASCADE",
         "RESTRICT",    "REFERENCES", "PRIMARY", "KEY",       "FOREIGN",  "UNIQUE",    "CHECK",
         "DEFAULT",     "CONSTRAINT", "IF",      "TEMPORARY", "INTO",     "VALUES",    "SET",
         "INSERT",      "UPDATE",     "DELETE",  "CREATE",    "DROP",     "ALTER",     "TABLE",
         "INDEX",       "VIEW",       "TRIGGER", "ADD",       "COLUMN",   "RENAME",    "TO",
         "OVER",        "PARTITION",  "EXCEPT",  "INTERSECT", "TRUE",     "FALSE",     "USING",
         "TRANSACTION", "GRANT",      "REVOKE",  "ANY",       "SOME",     "DATE",      "TIME",
         "TIMESTAMP",   "INTEGER",    "INT",     "BIGINT",    "SMALLINT", "DECIMAL",   "NUMERIC",
         "REAL",        "FLOAT",      "CHAR",    "VARCHAR",   "TEXT",     "REPLACE"});

    switch (type) {
    case DatabaseType::POSTGRESQL:
    case DatabaseType::REDSHIFT:
        append(kw, {"LIMIT",       "OFFSET",       "RETURNING",    "ILIKE",    "SIMILAR",
                    "LATERAL",     "MATERIALIZED", "CONCURRENTLY", "VACUUM",   "ANALYZE",
                    "COPY",        "ONLY",         "SCHEMA",       "DATABASE", "SEQUENCE",
                    "FUNCTION",    "PROCEDURE",    "RETURNS",      "LANGUAGE", "DECLARE",
                    "CONFLICT",    "DO",           "NOTHING",      "FILTER",   "WINDOW",
                    "FETCH",       "FIRST",        "NEXT",         "ROWS",     "ROW",
                    "GENERATED",   "ALWAYS",       "IDENTITY",     "STORED",   "SERIAL",
                    "BIGSERIAL",   "SMALLSERIAL",  "BOOLEAN",      "JSON",     "JSONB",
                    "UUID",        "BYTEA",        "ARRAY",        "DOUBLE",   "PRECISION",
                    "TIMESTAMPTZ", "EXTENSION",    "TRUNCATE",     "CALL",     "NULLS",
                    "INTERVAL",    "EXPLAIN",      "TABLESAMPLE",  "UNLOGGED", "EXECUTE",
                    "RETURN",      "OWNER",        "TYPE",         "ENUM",     "DOMAIN"});
        if (type == DatabaseType::REDSHIFT)
            append(kw, {"DISTKEY", "SORTKEY", "DISTSTYLE", "ENCODE", "UNLOAD"});
        break;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        append(kw,
               {"LIMIT",    "OFFSET",    "AUTO_INCREMENT", "ENGINE",    "CHARSET",     "COLLATE",
                "SHOW",     "DESCRIBE",  "DATABASES",      "TABLES",    "COLUMNS",     "USE",
                "IGNORE",   "DUPLICATE", "STRAIGHT_JOIN",  "REGEXP",    "RLIKE",       "UNSIGNED",
                "SIGNED",   "ZEROFILL",  "TINYINT",        "MEDIUMINT", "LONGTEXT",    "MEDIUMTEXT",
                "TINYTEXT", "BLOB",      "LONGBLOB",       "DATETIME",  "ENUM",        "JSON",
                "BOOLEAN",  "DOUBLE",    "PROCEDURE",      "FUNCTION",  "DATABASE",    "SCHEMA",
                "TRUNCATE", "CALL",      "EXPLAIN",        "DELIMITER", "LOCK",        "UNLOCK",
                "INTERVAL", "WINDOW",    "ROWS",           "DIV",       "XOR",         "SEPARATOR",
                "MODIFY",   "CHANGE",    "AFTER",          "FIRST",     "PROCESSLIST", "VARIABLES",
                "STATUS",   "GENERATED", "ALWAYS",         "STORED",    "VIRTUAL",     "DECLARE"});
        if (type == DatabaseType::MARIADB)
            append(kw, {"RETURNING", "SEQUENCE"});
        break;
    case DatabaseType::SQLITE:
        append(kw, {"LIMIT",     "OFFSET",    "AUTOINCREMENT", "PRAGMA", "GLOB",      "REGEXP",
                    "VACUUM",    "ANALYZE",   "ATTACH",        "DETACH", "WITHOUT",   "ROWID",
                    "CONFLICT",  "ABORT",     "FAIL",          "IGNORE", "RETURNING", "STRICT",
                    "VIRTUAL",   "EXPLAIN",   "QUERY",         "PLAN",   "INDEXED",   "BLOB",
                    "BOOLEAN",   "DATETIME",  "FILTER",        "WINDOW", "ROWS",      "NULLS",
                    "GENERATED", "ALWAYS",    "STORED",        "DO",     "NOTHING",   "DATABASE",
                    "IMMEDIATE", "EXCLUSIVE", "DEFERRED",      "REINDEX"});
        break;
    case DatabaseType::MSSQL:
        append(kw, {"TOP",
                    "OFFSET",
                    "FETCH",
                    "NEXT",
                    "ROWS",
                    "ONLY",
                    "IDENTITY",
                    "NOLOCK",
                    "OUTPUT",
                    "GO",
                    "EXEC",
                    "EXECUTE",
                    "DECLARE",
                    "MERGE",
                    "MATCHED",
                    "PIVOT",
                    "UNPIVOT",
                    "APPLY",
                    "PROCEDURE",
                    "PROC",
                    "FUNCTION",
                    "DATABASE",
                    "SCHEMA",
                    "TRUNCATE",
                    "NVARCHAR",
                    "NCHAR",
                    "NTEXT",
                    "BIT",
                    "DATETIME",
                    "DATETIME2",
                    "DATETIMEOFFSET",
                    "UNIQUEIDENTIFIER",
                    "MONEY",
                    "VARBINARY",
                    "IMAGE",
                    "PRINT",
                    "RAISERROR",
                    "TRY",
                    "CATCH",
                    "THROW",
                    "WHILE",
                    "RETURN",
                    "USE",
                    "CLUSTERED",
                    "NONCLUSTERED",
                    "INCLUDE",
                    "PERCENT",
                    "TIES",
                    "ROWS",
                    "RETURNS",
                    "TRAN",
                    "SEQUENCE"});
        break;
    case DatabaseType::ORACLE:
        append(kw, {"ROWNUM",    "ROWID",     "CONNECT",  "PRIOR",   "START",    "MINUS",
                    "DUAL",      "MERGE",     "MATCHED",  "NOCYCLE", "FETCH",    "FIRST",
                    "NEXT",      "ROWS",      "ONLY",     "OFFSET",  "NUMBER",   "VARCHAR2",
                    "NVARCHAR2", "CLOB",      "BLOB",     "RAW",     "SEQUENCE", "PROCEDURE",
                    "FUNCTION",  "PACKAGE",   "BODY",     "SYNONYM", "DECLARE",  "EXCEPTION",
                    "LOOP",      "RETURNING", "TRUNCATE", "PURGE",   "EXPLAIN",  "PLAN",
                    "NULLS",     "LEVEL",     "SIBLINGS", "SCHEMA",  "IDENTITY", "GENERATED",
                    "ALWAYS",    "RETURN",    "NOCOPY"});
        break;
    case DatabaseType::DUCKDB:
        append(kw, {"LIMIT",   "OFFSET",   "RETURNING", "ILIKE",       "SIMILAR",   "LATERAL",
                    "QUALIFY", "PIVOT",    "UNPIVOT",   "DESCRIBE",    "SUMMARIZE", "SHOW",
                    "EXCLUDE", "COPY",     "ATTACH",    "DETACH",      "ASOF",      "POSITIONAL",
                    "ANTI",    "SEMI",     "SAMPLE",    "INSTALL",     "LOAD",      "PRAGMA",
                    "EXPLAIN", "ANALYZE",  "SEQUENCE",  "SCHEMA",      "MACRO",     "STRUCT",
                    "LIST",    "MAP",      "BOOLEAN",   "DOUBLE",      "HUGEINT",   "UBIGINT",
                    "BLOB",    "UUID",     "JSON",      "TIMESTAMPTZ", "FILTER",    "WINDOW",
                    "NULLS",   "CONFLICT", "DO",        "NOTHING",     "GENERATED", "ALWAYS",
                    "VIRTUAL", "DATABASE", "INTERVAL",  "FUNCTION",    "TYPE",      "ENUM",
                    "USE"});
        break;
    default:
        break;
    }
    sortUnique(kw);
    return kw;
}

std::vector<std::string> buildFunctions(DatabaseType type) {
    std::vector<std::string> fn;
    if (!isSqlDialect(type))
        return fn;
    if (type == DatabaseType::CASSANDRA) {
        append(fn, {"COUNT",
                    "MIN",
                    "MAX",
                    "SUM",
                    "AVG",
                    "CAST",
                    "NOW",
                    "UUID",
                    "TOKEN",
                    "TTL",
                    "WRITETIME",
                    "TOTIMESTAMP",
                    "TODATE",
                    "TOUNIXTIMESTAMP",
                    "MINTIMEUUID",
                    "MAXTIMEUUID",
                    "CURRENTTIMESTAMP",
                    "CURRENTDATE",
                    "TOJSON",
                    "FROMJSON"});
        sortUnique(fn);
        return fn;
    }

    append(fn, {"COUNT",       "SUM",
                "AVG",         "MIN",
                "MAX",         "COALESCE",
                "NULLIF",      "CAST",
                "UPPER",       "LOWER",
                "TRIM",        "LTRIM",
                "RTRIM",       "REPLACE",
                "ABS",         "ROUND",
                "FLOOR",       "ROW_NUMBER",
                "RANK",        "DENSE_RANK",
                "NTILE",       "LAG",
                "LEAD",        "FIRST_VALUE",
                "LAST_VALUE",  "CURRENT_TIMESTAMP",
                "CURRENT_DATE"});

    switch (type) {
    case DatabaseType::POSTGRESQL:
    case DatabaseType::REDSHIFT:
        append(fn, {"STRING_AGG",
                    "ARRAY_AGG",
                    "BOOL_AND",
                    "BOOL_OR",
                    "JSON_AGG",
                    "JSONB_AGG",
                    "JSON_BUILD_OBJECT",
                    "JSONB_BUILD_OBJECT",
                    "JSON_BUILD_ARRAY",
                    "JSON_OBJECT_AGG",
                    "JSONB_SET",
                    "JSONB_EXTRACT_PATH",
                    "JSON_EXTRACT_PATH",
                    "TO_JSON",
                    "TO_JSONB",
                    "ROW_TO_JSON",
                    "LENGTH",
                    "CHAR_LENGTH",
                    "SUBSTRING",
                    "SUBSTR",
                    "POSITION",
                    "STRPOS",
                    "SPLIT_PART",
                    "INITCAP",
                    "LPAD",
                    "RPAD",
                    "LEFT",
                    "RIGHT",
                    "REVERSE",
                    "REPEAT",
                    "TRANSLATE",
                    "FORMAT",
                    "CONCAT",
                    "CONCAT_WS",
                    "REGEXP_REPLACE",
                    "REGEXP_MATCHES",
                    "REGEXP_SPLIT_TO_TABLE",
                    "NOW",
                    "CURRENT_TIME",
                    "CLOCK_TIMESTAMP",
                    "DATE_TRUNC",
                    "DATE_PART",
                    "EXTRACT",
                    "AGE",
                    "TO_CHAR",
                    "TO_DATE",
                    "TO_TIMESTAMP",
                    "TO_NUMBER",
                    "MAKE_DATE",
                    "MAKE_INTERVAL",
                    "GENERATE_SERIES",
                    "UNNEST",
                    "ARRAY_LENGTH",
                    "ARRAY_TO_STRING",
                    "STRING_TO_ARRAY",
                    "CARDINALITY",
                    "RANDOM",
                    "PI",
                    "CEIL",
                    "CEILING",
                    "MOD",
                    "POWER",
                    "SQRT",
                    "SIGN",
                    "EXP",
                    "LN",
                    "LOG",
                    "GREATEST",
                    "LEAST",
                    "GEN_RANDOM_UUID",
                    "NEXTVAL",
                    "CURRVAL",
                    "SETVAL",
                    "PG_SIZE_PRETTY",
                    "PG_TOTAL_RELATION_SIZE",
                    "PERCENT_RANK",
                    "CUME_DIST",
                    "NTH_VALUE",
                    "PERCENTILE_CONT",
                    "MODE"});
        if (type == DatabaseType::REDSHIFT)
            append(fn, {"LISTAGG", "GETDATE", "DATEADD", "DATEDIFF", "NVL", "NVL2", "DECODE"});
        break;
    case DatabaseType::MYSQL:
    case DatabaseType::MARIADB:
        append(fn, {"GROUP_CONCAT",
                    "JSON_ARRAYAGG",
                    "JSON_OBJECTAGG",
                    "LENGTH",
                    "CHAR_LENGTH",
                    "SUBSTRING",
                    "SUBSTR",
                    "SUBSTRING_INDEX",
                    "LOCATE",
                    "INSTR",
                    "POSITION",
                    "CONCAT",
                    "CONCAT_WS",
                    "LEFT",
                    "RIGHT",
                    "REVERSE",
                    "REPEAT",
                    "LPAD",
                    "RPAD",
                    "FORMAT",
                    "REGEXP_REPLACE",
                    "REGEXP_LIKE",
                    "NOW",
                    "CURDATE",
                    "CURTIME",
                    "SYSDATE",
                    "DATE",
                    "DATE_ADD",
                    "DATE_SUB",
                    "DATEDIFF",
                    "TIMESTAMPDIFF",
                    "DATE_FORMAT",
                    "STR_TO_DATE",
                    "UNIX_TIMESTAMP",
                    "FROM_UNIXTIME",
                    "YEAR",
                    "MONTH",
                    "DAY",
                    "HOUR",
                    "MINUTE",
                    "SECOND",
                    "EXTRACT",
                    "IFNULL",
                    "IF",
                    "ISNULL",
                    "CONVERT",
                    "JSON_EXTRACT",
                    "JSON_UNQUOTE",
                    "JSON_OBJECT",
                    "JSON_ARRAY",
                    "JSON_CONTAINS",
                    "JSON_SET",
                    "RAND",
                    "UUID",
                    "LAST_INSERT_ID",
                    "CEIL",
                    "CEILING",
                    "MOD",
                    "POWER",
                    "POW",
                    "SQRT",
                    "SIGN",
                    "EXP",
                    "LN",
                    "LOG",
                    "GREATEST",
                    "LEAST",
                    "PERCENT_RANK",
                    "CUME_DIST",
                    "NTH_VALUE"});
        break;
    case DatabaseType::SQLITE:
        append(fn, {"GROUP_CONCAT",
                    "TOTAL",
                    "LENGTH",
                    "SUBSTR",
                    "SUBSTRING",
                    "INSTR",
                    "CONCAT",
                    "PRINTF",
                    "FORMAT",
                    "HEX",
                    "QUOTE",
                    "TYPEOF",
                    "UNICODE",
                    "CHAR",
                    "ZEROBLOB",
                    "RANDOM",
                    "RANDOMBLOB",
                    "IFNULL",
                    "IIF",
                    "DATE",
                    "TIME",
                    "DATETIME",
                    "JULIANDAY",
                    "UNIXEPOCH",
                    "STRFTIME",
                    "JSON",
                    "JSON_EXTRACT",
                    "JSON_OBJECT",
                    "JSON_ARRAY",
                    "JSON_GROUP_ARRAY",
                    "JSON_GROUP_OBJECT",
                    "JSON_EACH",
                    "JSON_TREE",
                    "LAST_INSERT_ROWID",
                    "CHANGES",
                    "TOTAL_CHANGES",
                    "LIKELIHOOD",
                    "PERCENT_RANK",
                    "CUME_DIST",
                    "NTH_VALUE",
                    "CEIL",
                    "CEILING",
                    "MOD",
                    "POWER",
                    "SQRT",
                    "SIGN",
                    "EXP",
                    "LN",
                    "LOG"});
        break;
    case DatabaseType::MSSQL:
        append(fn, {"LEN",         "DATALENGTH",
                    "SUBSTRING",   "CHARINDEX",
                    "PATINDEX",    "CONCAT",
                    "CONCAT_WS",   "LEFT",
                    "RIGHT",       "REVERSE",
                    "REPLICATE",   "STUFF",
                    "STRING_AGG",  "STRING_SPLIT",
                    "FORMAT",      "CONVERT",
                    "TRY_CAST",    "TRY_CONVERT",
                    "PARSE",       "ISNULL",
                    "IIF",         "CHOOSE",
                    "GETDATE",     "GETUTCDATE",
                    "SYSDATETIME", "SYSUTCDATETIME",
                    "DATEADD",     "DATEDIFF",
                    "DATEPART",    "DATENAME",
                    "EOMONTH",     "YEAR",
                    "MONTH",       "DAY",
                    "NEWID",       "SCOPE_IDENTITY",
                    "OBJECT_ID",   "OBJECT_NAME",
                    "DB_NAME",     "JSON_VALUE",
                    "JSON_QUERY",  "JSON_MODIFY",
                    "OPENJSON",    "ISJSON",
                    "CEILING",     "POWER",
                    "SQRT",        "SIGN",
                    "EXP",         "LOG",
                    "RAND",        "PERCENT_RANK",
                    "CUME_DIST",   "GREATEST",
                    "LEAST"});
        break;
    case DatabaseType::ORACLE:
        append(fn, {"NVL",
                    "NVL2",
                    "DECODE",
                    "LENGTH",
                    "SUBSTR",
                    "INSTR",
                    "CONCAT",
                    "LPAD",
                    "RPAD",
                    "INITCAP",
                    "TRANSLATE",
                    "REGEXP_LIKE",
                    "REGEXP_REPLACE",
                    "REGEXP_SUBSTR",
                    "REGEXP_INSTR",
                    "LISTAGG",
                    "SYSDATE",
                    "SYSTIMESTAMP",
                    "TO_CHAR",
                    "TO_DATE",
                    "TO_NUMBER",
                    "TO_TIMESTAMP",
                    "ADD_MONTHS",
                    "MONTHS_BETWEEN",
                    "LAST_DAY",
                    "NEXT_DAY",
                    "TRUNC",
                    "EXTRACT",
                    "CEIL",
                    "MOD",
                    "POWER",
                    "SQRT",
                    "SIGN",
                    "EXP",
                    "LN",
                    "LOG",
                    "GREATEST",
                    "LEAST",
                    "SYS_GUID",
                    "JSON_VALUE",
                    "JSON_QUERY",
                    "JSON_OBJECT",
                    "JSON_ARRAYAGG",
                    "PERCENT_RANK",
                    "CUME_DIST",
                    "NTH_VALUE"});
        break;
    case DatabaseType::DUCKDB:
        append(fn, {"STRING_AGG",
                    "LIST",
                    "ARRAY_AGG",
                    "ANY_VALUE",
                    "ARG_MAX",
                    "ARG_MIN",
                    "MEDIAN",
                    "QUANTILE_CONT",
                    "MODE",
                    "APPROX_COUNT_DISTINCT",
                    "LENGTH",
                    "SUBSTRING",
                    "SUBSTR",
                    "STRPOS",
                    "INSTR",
                    "CONCAT",
                    "CONCAT_WS",
                    "LEFT",
                    "RIGHT",
                    "REVERSE",
                    "REPEAT",
                    "LPAD",
                    "RPAD",
                    "SPLIT_PART",
                    "STRING_SPLIT",
                    "REGEXP_MATCHES",
                    "REGEXP_REPLACE",
                    "REGEXP_EXTRACT",
                    "NOW",
                    "TODAY",
                    "DATE_TRUNC",
                    "DATE_PART",
                    "DATEDIFF",
                    "DATE_DIFF",
                    "EXTRACT",
                    "STRFTIME",
                    "STRPTIME",
                    "EPOCH",
                    "AGE",
                    "MAKE_DATE",
                    "GENERATE_SERIES",
                    "RANGE",
                    "UNNEST",
                    "LIST_VALUE",
                    "STRUCT_PACK",
                    "READ_CSV",
                    "READ_CSV_AUTO",
                    "READ_PARQUET",
                    "READ_JSON",
                    "READ_JSON_AUTO",
                    "IFNULL",
                    "CEIL",
                    "CEILING",
                    "MOD",
                    "POWER",
                    "SQRT",
                    "SIGN",
                    "EXP",
                    "LN",
                    "LOG",
                    "GREATEST",
                    "LEAST",
                    "RANDOM",
                    "UUID",
                    "GEN_RANDOM_UUID"});
        break;
    default:
        break;
    }
    sortUnique(fn);
    return fn;
}

// words that are reserved enough to need quoting as identifiers
const WordSet& reservedWords() {
    static const WordSet words = {
        "ALL",          "AND",          "ANY",          "AS",
        "ASC",          "BETWEEN",      "BY",           "CASE",
        "CHECK",        "COLUMN",       "CONSTRAINT",   "CREATE",
        "CROSS",        "CURRENT_DATE", "CURRENT_TIME", "CURRENT_TIMESTAMP",
        "CURRENT_USER", "DEFAULT",      "DELETE",       "DESC",
        "DISTINCT",     "DROP",         "ELSE",         "END",
        "EXCEPT",       "EXISTS",       "FALSE",        "FETCH",
        "FOR",          "FOREIGN",      "FROM",         "FULL",
        "GRANT",        "GROUP",        "HAVING",       "IN",
        "INDEX",        "INNER",        "INSERT",       "INTERSECT",
        "INTO",         "IS",           "JOIN",         "KEY",
        "LEFT",         "LIKE",         "LIMIT",        "NATURAL",
        "NOT",          "NULL",         "OFFSET",       "ON",
        "OR",           "ORDER",        "OUTER",        "PRIMARY",
        "REFERENCES",   "RIGHT",        "SELECT",       "SET",
        "SOME",         "TABLE",        "THEN",         "TO",
        "TRUE",         "UNION",        "UNIQUE",       "UPDATE",
        "USER",         "USING",        "VALUES",       "WHEN",
        "WHERE",        "WINDOW",       "WITH"};
    return words;
}

// words that end a relation reference: never an implicit alias
const WordSet& notAliasWords() {
    static const WordSet words = {
        "WHERE",     "ON",        "USING",         "JOIN",       "INNER",
        "LEFT",      "RIGHT",     "FULL",          "OUTER",      "CROSS",
        "NATURAL",   "SET",       "VALUES",        "GROUP",      "ORDER",
        "HAVING",    "LIMIT",     "OFFSET",        "UNION",      "INTERSECT",
        "EXCEPT",    "MINUS",     "WINDOW",        "FETCH",      "FOR",
        "RETURNING", "SELECT",    "FROM",          "AS",         "WITH",
        "LATERAL",   "APPLY",     "STRAIGHT_JOIN", "QUALIFY",    "PIVOT",
        "UNPIVOT",   "SAMPLE",    "TABLESAMPLE",   "START",      "CONNECT",
        "DEFAULT",   "OUTPUT",    "ASOF",          "POSITIONAL", "ANTI",
        "SEMI",      "PARTITION", "INTO",          "WHEN",       "THEN",
        "ELSE",      "END",       "AND",           "OR",         "NOT",
        "IS",        "IN",        "LIKE",          "ILIKE",      "BETWEEN",
        "IF",        "ALLOW",     "USE",           "IGNORE",     "FORCE",
        "NULL",      "TRUE",      "FALSE",         "LOCK",       "ONLY",
        "DO",        "TOP"};
    return words;
}

bool inSet(const WordSet& set, std::string_view word) {
    return set.contains(upper(word));
}

// ---------- matching and scoring ----------

constexpr int kTierPrefix = 4000;
constexpr int kExactCaseBonus = 150;
constexpr int kTierBoundary = 3000;
constexpr int kTierSubstring = 2000;
constexpr int kTierFuzzy = 1000;
constexpr int kTierEmpty = 4000;

int matchTier(std::string_view label, std::string_view prefix, bool fuzzy) {
    if (prefix.empty())
        return kTierEmpty;
    if (label.starts_with(prefix))
        return kTierPrefix + kExactCaseBonus;
    if (istartsWith(label, prefix))
        return kTierPrefix;
    for (size_t pos = ifind(label, prefix); pos != std::string_view::npos;
         pos = ifind(label, prefix, pos + 1)) {
        const char before = label[pos - 1]; // pos > 0: prefix match handled above
        if (before == '_' || before == '.' || before == ' ' ||
            (std::isupper(static_cast<unsigned char>(label[pos])) &&
             std::islower(static_cast<unsigned char>(before))))
            return kTierBoundary;
    }
    // keywords and functions stop at word boundaries: "al" must not offer DECIMAL
    if (!fuzzy)
        return -1;
    if (prefix.size() >= 2 && ifind(label, prefix) != std::string_view::npos)
        return kTierSubstring;
    if (prefix.size() >= 2 && isSubsequence(label, prefix))
        return kTierFuzzy;
    return -1;
}

// ---------- analyzer ----------

struct ColInfo {
    std::string name;
    std::string type;
    const Column* column = nullptr;
};

struct RelationRef {
    std::string name;              // alias, else last path part
    std::string alias;             // explicit alias, may be empty
    std::vector<std::string> path; // empty for derived tables
    int group = 0;                 // scope the reference lives in
    int derived = -1;              // subquery group of a derived table
    bool function = false;         // table function in FROM
    std::vector<std::string> columnAliases;
};

struct Cte {
    std::string name;
    std::vector<std::string> columns;
    int body = -1;
};

struct Group {
    int parent = -1;
    int open = -1;  // index of '(' in toks, -1 for the root
    int close = -1; // index of ')', toks.size() when unclosed
};

struct Relation {
    const Table* table = nullptr;
    bool isView = false;
};

enum class Ctx : uint8_t {
    None,           // nothing to offer (naming something new)
    StatementStart, // first word of a statement
    Keyword,        // keywords only
    Relation,       // a table/view name slot
    AfterRelation,  // after a complete FROM item: JOIN, WHERE...
    Column,         // an expression: columns, functions, keywords
    TargetColumns,  // INSERT INTO t (|  /  CREATE INDEX ... ON t (|
};

struct Context {
    Ctx kind = Ctx::Keyword;
    std::vector<std::string> target; // TargetColumns relation path
};

class Analyzer {
public:
    Analyzer(std::string_view sql, size_t offset, const CompletionCatalog& catalog,
             DatabaseType type)
        : sql_(sql), catalog_(catalog), type_(type), dialect_(dialectFor(type)) {
        all_ = tokenize(sql, dialect_);
        span_ = statementAt(all_, offset, sql.size());
        for (size_t i = span_.firstTok; i < span_.lastTok; ++i) {
            if (all_[i].kind != Tok::Comment)
                toks_.push_back(all_[i]);
        }
        buildGroups();
        collectCtes();
        collectRefs();
    }

    const std::vector<Token>& allTokens() const {
        return all_;
    }
    const std::vector<Token>& toks() const {
        return toks_;
    }
    const StatementSpan& span() const {
        return span_;
    }
    DatabaseType type() const {
        return type_;
    }

    // ----- token predicates -----

    bool isPunct(int i, char c) const {
        return i >= 0 && i < size() && toks_[i].kind == Tok::Punct && toks_[i].text[0] == c;
    }

    bool isKw(int i, std::string_view kw) const {
        return i >= 0 && i < size() && toks_[i].kind == Tok::Word && ieq(toks_[i].text, kw);
    }

    bool isKwIn(int i, std::initializer_list<std::string_view> kws) const {
        for (auto kw : kws) {
            if (isKw(i, kw))
                return true;
        }
        return false;
    }

    bool isIdent(int i) const {
        if (i < 0 || i >= size())
            return false;
        return toks_[i].kind == Tok::Word || toks_[i].kind == Tok::QuotedIdent;
    }

    // an identifier that can name a relation (not a clause keyword)
    bool isNameIdent(int i) const {
        if (!isIdent(i))
            return false;
        return toks_[i].kind == Tok::QuotedIdent || !inSet(notAliasWords(), toks_[i].text);
    }

    int size() const {
        return static_cast<int>(toks_.size());
    }

    int groupOf(int i) const {
        return groupOf_[static_cast<size_t>(i)];
    }

    int closeOf(int openIdx) const {
        const int g = opens_[static_cast<size_t>(openIdx)];
        return g < 0 ? openIdx : groups_[static_cast<size_t>(g)].close;
    }

    bool isAncestorOrSelf(int ancestor, int g) const {
        while (g >= 0) {
            if (g == ancestor)
                return true;
            g = groups_[static_cast<size_t>(g)].parent;
        }
        return false;
    }

    // group the insertion point before token `index` sits in
    int groupAt(int index) const {
        std::vector<int> stack{0};
        for (int i = 0; i < index && i < size(); ++i) {
            if (isPunct(i, '('))
                stack.push_back(opens_[static_cast<size_t>(i)]);
            else if (isPunct(i, ')') && stack.size() > 1)
                stack.pop_back();
        }
        return stack.back();
    }

    bool statementStartsWith(std::string_view kw) const {
        return isKw(0, kw);
    }

    // ----- relation lookup -----

    static bool qualifierMatches(std::string_view schemaPath,
                                 const std::vector<std::string>& qualifier) {
        if (qualifier.empty())
            return true;
        const auto segs = splitPath(schemaPath);
        if (segs.size() < qualifier.size())
            return false;
        const size_t off = segs.size() - qualifier.size();
        for (size_t i = 0; i < qualifier.size(); ++i) {
            if (!ieq(segs[off + i], qualifier[i]))
                return false;
        }
        return true;
    }

    Relation findRelation(const std::vector<std::string>& path) const {
        if (path.empty())
            return {};
        const std::string& name = path.back();
        const std::vector<std::string> qual(path.begin(), path.end() - 1);
        Relation best;
        int bestRank = -1;
        auto consider = [&](const std::vector<Table>& list, bool isView) {
            for (const auto& t : list) {
                if (!ieq(t.name, name) || !qualifierMatches(t.schema, qual))
                    continue;
                int rank = 0;
                if (t.name == name)
                    rank += 1;
                if (ieq(t.schema, catalog_.defaultSchema))
                    rank += 4;
                else if (t.schema.empty())
                    rank += 2;
                if (rank > bestRank) {
                    bestRank = rank;
                    best = {&t, isView};
                }
            }
        };
        consider(catalog_.tables, false);
        consider(catalog_.views, true);
        return best;
    }

    const Cte* findCte(std::string_view name) const {
        for (const auto& c : ctes_) {
            if (ieq(c.name, name))
                return &c;
        }
        return nullptr;
    }

    // closest visible reference named `name` from group `g`
    const RelationRef* findRef(std::string_view name, int g) const {
        const RelationRef* best = nullptr;
        int bestDepth = -1;
        for (const auto& r : refs_) {
            if (!ieq(r.name, name) || !isAncestorOrSelf(r.group, g))
                continue;
            const int d = depthOf(r.group);
            if (d > bestDepth) {
                bestDepth = d;
                best = &r;
            }
        }
        return best;
    }

    std::vector<const RelationRef*> visibleRefs(int g) const {
        std::vector<const RelationRef*> out;
        for (const auto& r : refs_) {
            if (isAncestorOrSelf(r.group, g))
                out.push_back(&r);
        }
        // nearest scope first
        std::ranges::stable_sort(out, [&](const RelationRef* a, const RelationRef* b) {
            return depthOf(a->group) > depthOf(b->group);
        });
        return out;
    }

    static std::vector<ColInfo> tableColumns(const Table& t) {
        std::vector<ColInfo> cols;
        cols.reserve(t.columns.size());
        for (const auto& c : t.columns)
            cols.push_back({c.name, c.type, &c});
        return cols;
    }

    static void applyAliases(std::vector<ColInfo>& cols, const std::vector<std::string>& aliases) {
        if (aliases.empty())
            return;
        for (size_t i = 0; i < aliases.size(); ++i) {
            if (i < cols.size()) {
                cols[i].name = aliases[i];
                cols[i].column = nullptr;
            } else {
                cols.push_back({aliases[i], {}, nullptr});
            }
        }
    }

    std::vector<ColInfo> cteColumns(const Cte& c, int depth) const {
        std::vector<ColInfo> cols;
        if (c.body >= 0)
            cols = selectColumns(c.body, depth + 1);
        applyAliases(cols, c.columns);
        return cols;
    }

    std::vector<ColInfo> refColumns(const RelationRef& r, int depth = 0) const {
        if (depth > 6)
            return {};
        std::vector<ColInfo> cols;
        if (r.derived >= 0) {
            cols = selectColumns(r.derived, depth + 1);
        } else if (r.function) {
            // columns only through an alias list
        } else if (const Cte* c = r.path.size() == 1 ? findCte(r.path[0]) : nullptr) {
            cols = cteColumns(*c, depth);
        } else if (auto rel = findRelation(r.path); rel.table) {
            cols = tableColumns(*rel.table);
        }
        applyAliases(cols, r.columnAliases);
        return cols;
    }

    // ----- select list of a group (ctes, derived tables) -----

    std::vector<ColInfo> selectColumns(int g, int depth) const {
        std::vector<ColInfo> out;
        if (depth > 6)
            return out;
        const auto& grp = groups_[static_cast<size_t>(g)];
        const int begin = grp.open + 1;
        const int end = std::min(grp.close, size());
        int k = begin;
        while (k < end && !(groupOf(k) == g && isKw(k, "SELECT")))
            ++k;
        if (k >= end)
            return out;
        ++k;
        // DISTINCT [ON (...)], ALL, TOP n [PERCENT]
        while (k < end) {
            if (isKwIn(k, {"DISTINCT", "ALL"})) {
                ++k;
                if (isKw(k, "ON") && isPunct(k + 1, '('))
                    k = closeOf(k + 1) + 1;
            } else if (isKw(k, "TOP")) {
                ++k;
                if (isPunct(k, '('))
                    k = closeOf(k) + 1;
                else
                    ++k;
                if (isKw(k, "PERCENT"))
                    ++k;
            } else {
                break;
            }
        }

        std::vector<int> item; // element indices at this level
        auto flush = [&] {
            deriveItem(item, g, depth, out);
            item.clear();
        };
        while (k < end) {
            if (isKwIn(k, {"FROM", "INTO", "WHERE", "GROUP", "ORDER", "HAVING", "LIMIT", "UNION",
                           "EXCEPT", "INTERSECT", "WINDOW", "FETCH", "OFFSET"}))
                break;
            if (isPunct(k, ',')) {
                flush();
                ++k;
                continue;
            }
            item.push_back(k);
            k = isPunct(k, '(') ? closeOf(k) + 1 : k + 1;
        }
        flush();
        return out;
    }

    void deriveItem(const std::vector<int>& item, int g, int depth,
                    std::vector<ColInfo>& out) const {
        if (item.empty())
            return;
        const int last = item.back();
        const int n = static_cast<int>(item.size());

        // * and t.*
        if (toks_[last].kind == Tok::Op && toks_[last].text == "*") {
            if (n == 1) {
                for (const auto* r : visibleRefsIn(g)) {
                    auto cols = refColumns(*r, depth + 1);
                    out.insert(out.end(), cols.begin(), cols.end());
                }
            } else if (n >= 3 && isPunct(item[n - 2], '.') && isIdent(item[n - 3])) {
                if (const auto* r = findRefIn(toks_[item[n - 3]].text, g)) {
                    auto cols = refColumns(*r, depth + 1);
                    out.insert(out.end(), cols.begin(), cols.end());
                }
            }
            return;
        }
        if (!isIdent(last))
            return;

        const std::string& name = toks_[last].text;
        if (n >= 2 && isKw(item[n - 2], "AS")) {
            out.push_back({name, {}, nullptr});
            return;
        }
        // plain or qualified column: a | t.a | s.t.a
        bool qualified = true;
        for (int i = 0; i < n; ++i) {
            const bool wantIdent = (i % 2) == 0;
            if (wantIdent ? !isIdent(item[i]) : !isPunct(item[i], '.')) {
                qualified = false;
                break;
            }
        }
        if (qualified && (n % 2) == 1) {
            ColInfo info{name, {}, nullptr};
            const RelationRef* owner = nullptr;
            if (n >= 3)
                owner = findRefIn(toks_[item[n - 3]].text, g);
            std::vector<const RelationRef*> candidates;
            if (owner)
                candidates.push_back(owner);
            else if (n == 1)
                candidates = visibleRefsIn(g);
            for (const auto* r : candidates) {
                for (const auto& c : refColumns(*r, depth + 1)) {
                    if (ieq(c.name, name)) {
                        info.type = c.type;
                        info.column = c.column;
                        break;
                    }
                }
                if (!info.type.empty() || info.column)
                    break;
            }
            out.push_back(std::move(info));
            return;
        }
        // implicit alias: expr name
        if (n >= 2 && toks_[last].kind == Tok::Word && inSet(notAliasWords(), name))
            return;
        if (n >= 2 && !isPunct(item[n - 2], '.') && toks_[item[n - 2]].kind != Tok::Op)
            out.push_back({name, {}, nullptr});
    }

    // refs declared directly in group g (a select's own FROM)
    std::vector<const RelationRef*> visibleRefsIn(int g) const {
        std::vector<const RelationRef*> out;
        for (const auto& r : refs_) {
            if (r.group == g)
                out.push_back(&r);
        }
        return out;
    }

    const RelationRef* findRefIn(std::string_view name, int g) const {
        for (const auto& r : refs_) {
            if (r.group == g && ieq(r.name, name))
                return &r;
        }
        return findRef(name, g);
    }

    // ----- context -----

    // path of identifiers ending right before token `i` (exclusive): a.b.
    std::vector<std::string> qualifierBefore(int i, int& chainStart) const {
        std::vector<std::string> parts;
        chainStart = i;
        int j = i - 1;
        while (isPunct(j, '.') && isIdent(j - 1)) {
            parts.insert(parts.begin(), toks_[j - 1].text);
            chainStart = j - 1;
            j -= 2;
        }
        return parts;
    }

    // FROM that introduces relations (not EXTRACT(x FROM y), IS DISTINCT FROM)
    bool isTableFrom(int i) const {
        if (isKw(i - 1, "DISTINCT"))
            return false;
        const int g = groupOf(i);
        if (g > 0) {
            const int open = groups_[static_cast<size_t>(g)].open;
            if (isKwIn(open - 1, {"EXTRACT", "SUBSTRING", "SUBSTR", "TRIM", "OVERLAY", "POSITION"}))
                return false;
        }
        return true;
    }

    bool isRelationKeyword(int i) const {
        if (isKw(i, "FROM"))
            return isTableFrom(i);
        if (isKw(i, "USING"))
            return !isPunct(i + 1, '(');
        return isKwIn(i, {"JOIN", "UPDATE", "INTO", "TABLE", "STRAIGHT_JOIN"});
    }

    // TABLE right after CREATE [OR REPLACE] [TEMP...]: a new name
    bool isCreateTable(int i) const {
        int j = i - 1;
        while (isKwIn(j, {"TEMP", "TEMPORARY", "UNLOGGED", "GLOBAL", "LOCAL", "VIRTUAL", "EXTERNAL",
                          "REPLACE", "OR"}))
            --j;
        return isKw(j, "CREATE");
    }

    Context detectContext(int start) const {
        if (isKw(start - 1, "AS")) {
            const int g = groupOf(start - 1);
            if (g > 0 && isKwIn(groups_[static_cast<size_t>(g)].open - 1, {"CAST", "TRY_CAST"}))
                return {Ctx::Keyword, {}};
            if (statementStartsWith("CREATE") || statementStartsWith("ALTER"))
                return {Ctx::Keyword, {}};
            return {Ctx::None, {}};
        }

        int depth = 0;
        bool crossed = false;
        for (int k = start - 1; k >= 0; --k) {
            if (isPunct(k, ')')) {
                ++depth;
                continue;
            }
            if (isPunct(k, '(')) {
                if (depth > 0) {
                    --depth;
                    continue;
                }
                if (!crossed) {
                    crossed = true;
                    int chainStart = k;
                    std::vector<std::string> path;
                    if (isIdent(k - 1)) {
                        path = qualifierBefore(k - 1, chainStart);
                        path.push_back(toks_[k - 1].text);
                        if (chainStart == k)
                            chainStart = k - 1;
                    }
                    if (!path.empty() && isKw(chainStart - 1, "INTO"))
                        return {Ctx::TargetColumns, path};
                    if (!path.empty() && isKw(chainStart - 1, "ON") &&
                        statementStartsWith("CREATE"))
                        return {Ctx::TargetColumns, path};
                    if (isKw(k - 1, "USING"))
                        return {Ctx::Column, {}};
                }
                continue;
            }
            if (depth > 0 || toks_[k].kind != Tok::Word)
                continue;

            if (isRelationKeyword(k)) {
                if (isKw(k, "TABLE") && isCreateTable(k))
                    return {Ctx::None, {}};
                if (crossed)
                    return {Ctx::Keyword, {}};
                return {relationSlotEmpty(k, start) ? Ctx::Relation : Ctx::AfterRelation, {}};
            }
            if (isKwIn(k, {"SELECT", "WHERE", "ON", "HAVING", "BY", "SET", "RETURNING", "VALUES",
                           "OUTPUT", "QUALIFY", "WHEN", "THEN", "ELSE"}))
                return {Ctx::Column, {}};
            if (isKwIn(k, {"LIMIT", "OFFSET", "FETCH", "TOP"}))
                return {Ctx::Keyword, {}};
        }
        if (crossed)
            return {Ctx::Keyword, {}};
        return {start == 0 ? Ctx::StatementStart : Ctx::Keyword, {}};
    }

    // nothing but the start of a name between relation keyword `kw` and `start`
    bool relationSlotEmpty(int kw, int start) const {
        const bool list = isKwIn(kw, {"FROM", "USING"});
        int count = 0;
        for (int k = kw + 1; k < start; ++k) {
            if (isPunct(k, '(')) {
                ++count;
                k = closeOf(k);
                continue;
            }
            if (list && isPunct(k, ',')) {
                count = 0;
                continue;
            }
            if (isKwIn(k, {"LATERAL", "ONLY", "IF", "NOT", "EXISTS"}))
                continue;
            ++count;
        }
        return count == 0;
    }

    const std::vector<Cte>& ctes() const {
        return ctes_;
    }
    const std::vector<RelationRef>& refs() const {
        return refs_;
    }

private:
    int depthOf(int g) const {
        int d = 0;
        while (g > 0) {
            g = groups_[static_cast<size_t>(g)].parent;
            ++d;
        }
        return d;
    }

    void buildGroups() {
        groupOf_.assign(toks_.size(), 0);
        opens_.assign(toks_.size(), -1);
        groups_.push_back({-1, -1, size()});
        std::vector<int> stack{0};
        for (int i = 0; i < size(); ++i) {
            if (isPunct(i, '(')) {
                groupOf_[static_cast<size_t>(i)] = stack.back();
                const int g = static_cast<int>(groups_.size());
                groups_.push_back({stack.back(), i, size()});
                opens_[static_cast<size_t>(i)] = g;
                stack.push_back(g);
            } else if (isPunct(i, ')')) {
                if (stack.size() > 1) {
                    groups_[static_cast<size_t>(stack.back())].close = i;
                    stack.pop_back();
                }
                groupOf_[static_cast<size_t>(i)] = stack.back();
            } else {
                groupOf_[static_cast<size_t>(i)] = stack.back();
            }
        }
    }

    std::vector<std::string> identsInGroup(int openIdx) const {
        std::vector<std::string> out;
        const int g = opens_[static_cast<size_t>(openIdx)];
        if (g < 0)
            return out;
        for (int k = openIdx + 1; k < closeOf(openIdx); ++k) {
            if (groupOf(k) == g && isIdent(k))
                out.push_back(toks_[k].text);
        }
        return out;
    }

    void collectCtes() {
        for (int i = 0; i < size(); ++i) {
            if (!isKw(i, "WITH"))
                continue;
            int j = i + 1;
            if (isKw(j, "RECURSIVE"))
                ++j;
            while (isIdent(j)) {
                Cte c;
                c.name = toks_[j].text;
                ++j;
                if (isPunct(j, '(')) {
                    c.columns = identsInGroup(j);
                    j = closeOf(j) + 1;
                }
                if (!isKw(j, "AS"))
                    break;
                ++j;
                if (isKw(j, "NOT"))
                    ++j;
                if (isKw(j, "MATERIALIZED"))
                    ++j;
                if (!isPunct(j, '('))
                    break;
                c.body = opens_[static_cast<size_t>(j)];
                j = closeOf(j) + 1;
                ctes_.push_back(std::move(c));
                if (!isPunct(j, ','))
                    break;
                ++j;
            }
        }
    }

    void collectRefs() {
        for (int i = 0; i < size(); ++i) {
            if (toks_[i].kind != Tok::Word || !isRelationKeyword(i))
                continue;
            if (isKw(i, "TABLE") && isCreateTable(i))
                continue;
            parseRelationList(i);
        }
    }

    void parseRelationList(int kw) {
        const bool list = isKwIn(kw, {"FROM", "USING"});
        const bool into = isKw(kw, "INTO");
        const int g = groupOf(kw);
        int j = kw + 1;
        while (j < size() && groupOf(j) == g) {
            while (isKwIn(j, {"LATERAL", "ONLY", "IF", "NOT", "EXISTS"}))
                ++j;
            RelationRef r;
            r.group = g;
            if (isPunct(j, '(')) {
                r.derived = opens_[static_cast<size_t>(j)];
                j = closeOf(j) + 1;
            } else if (isNameIdent(j)) {
                r.path.push_back(toks_[j].text);
                ++j;
                while (isPunct(j, '.') && isIdent(j + 1)) {
                    r.path.push_back(toks_[j + 1].text);
                    j += 2;
                }
                if (!into && isPunct(j, '(')) {
                    r.function = true;
                    j = closeOf(j) + 1;
                }
            } else {
                break;
            }

            if (isKw(j, "AS")) {
                if (isIdent(j + 1)) {
                    r.alias = toks_[j + 1].text;
                    j += 2;
                } else {
                    ++j;
                }
            } else if (!into && isNameIdent(j)) {
                r.alias = toks_[j].text;
                ++j;
            }
            if (!r.alias.empty() && isPunct(j, '(') && (r.derived >= 0 || r.function)) {
                r.columnAliases = identsInGroup(j);
                j = closeOf(j) + 1;
            }
            r.name = !r.alias.empty() ? r.alias : (r.path.empty() ? "" : r.path.back());
            if (!r.name.empty())
                refs_.push_back(std::move(r));

            if (list && isPunct(j, ',')) {
                ++j;
                continue;
            }
            break;
        }
    }

    std::string_view sql_;
    const CompletionCatalog& catalog_;
    DatabaseType type_;
    Dialect dialect_;
    std::vector<Token> all_;
    std::vector<Token> toks_;
    StatementSpan span_;
    std::vector<Group> groups_;
    std::vector<int> groupOf_;
    std::vector<int> opens_; // '(' token -> group it opens
    std::vector<Cte> ctes_;
    std::vector<RelationRef> refs_;
};

// ---------- item emission ----------

// schema path as typed relative to the default: shared leading segments go
std::string relativeSchema(std::string_view schema, std::string_view def) {
    auto segs = splitPath(schema);
    const auto defSegs = splitPath(def);
    size_t common = 0;
    while (common < segs.size() && common < defSegs.size() && common + 1 < segs.size() &&
           ieq(segs[common], defSegs[common]))
        ++common;
    std::string out;
    for (size_t i = common; i < segs.size(); ++i) {
        if (!out.empty())
            out += '.';
        out += segs[i];
    }
    return out;
}

class Emitter {
public:
    Emitter(std::string prefix, bool quoted, DatabaseType type)
        : prefix_(std::move(prefix)), quoted_(quoted), type_(type), builder_(builderFor(type)) {}

    // keywords follow the case the user is typing in
    std::string keyword(std::string_view kw) const {
        bool alpha = false;
        for (const char c : prefix_) {
            if (std::isupper(static_cast<unsigned char>(c)))
                return std::string(kw);
            alpha = alpha || std::isalpha(static_cast<unsigned char>(c));
        }
        return alpha ? lower(kw) : std::string(kw);
    }

    // quoted mode inserts names always quoted, as the user started a quote
    std::string name(std::string_view n) const {
        return quoted_ ? builder_.quoteIdentifier(std::string(n))
                       : quoteIdentifierIfNeeded(n, type_);
    }

    std::string path(std::string_view schema, std::string_view n) const {
        if (schema.empty())
            return name(n);
        std::string out;
        for (const auto& seg : splitPath(schema)) {
            out += quoted_ ? builder_.quoteIdentifier(seg) : quoteIdentifierIfNeeded(seg, type_);
            out += '.';
        }
        return out + name(n);
    }

    void add(std::string label, CompletionKind kind, std::string detail, std::string owner,
             std::string insert, int bonus, bool fuzzy = true) {
        if (quoted_ && (kind == CompletionKind::Keyword || kind == CompletionKind::Function))
            return;
        const int tier = matchTier(label, prefix_, fuzzy);
        if (tier < 0)
            return;
        if (!prefix_.empty() && ieq(label, prefix_) && !quoted_)
            return;
        items_.push_back({std::move(label), kind, std::move(detail), std::move(owner),
                          std::move(insert), tier + bonus});
    }

    std::vector<CompletionItem> take() {
        std::ranges::stable_sort(items_, [](const CompletionItem& a, const CompletionItem& b) {
            if (a.score != b.score)
                return a.score > b.score;
            if (!ieq(a.label, b.label))
                return iless(a.label, b.label);
            if (a.kind != b.kind)
                return a.kind < b.kind;
            return iless(a.owner, b.owner);
        });
        std::vector<CompletionItem> out;
        std::unordered_set<std::string> seen;
        for (auto& it : items_) {
            std::string key = std::to_string(static_cast<int>(it.kind)) + '\x1f' + it.label +
                              '\x1f' + it.insertText + '\x1f' + it.owner;
            if (seen.insert(std::move(key)).second)
                out.push_back(std::move(it));
        }
        return out;
    }

private:
    std::string prefix_;
    bool quoted_;
    DatabaseType type_;
    const ISQLBuilder& builder_;
    std::vector<CompletionItem> items_;
};

// keywords that continue a statement after a FROM item
const WordSet& clauseKeywords() {
    static const WordSet words = {
        "WHERE",     "JOIN",   "INNER",  "LEFT",      "RIGHT",  "FULL",    "CROSS",
        "OUTER",     "ON",     "USING",  "AS",        "GROUP",  "ORDER",   "HAVING",
        "LIMIT",     "UNION",  "SET",    "VALUES",    "SELECT", "NATURAL", "OFFSET",
        "RETURNING", "WINDOW", "EXCEPT", "INTERSECT", "FETCH",  "QUALIFY", "DEFAULT"};
    return words;
}

const WordSet& statementKeywords() {
    static const WordSet words = {
        "SELECT", "INSERT",   "UPDATE",   "DELETE",  "WITH",   "CREATE",   "ALTER",
        "DROP",   "TRUNCATE", "EXPLAIN",  "BEGIN",   "COMMIT", "ROLLBACK", "GRANT",
        "REVOKE", "SHOW",     "DESCRIBE", "USE",     "PRAGMA", "VACUUM",   "ANALYZE",
        "CALL",   "EXEC",     "EXECUTE",  "DECLARE", "MERGE",  "SET",      "COPY"};
    return words;
}

void addKeywords(Emitter& em, DatabaseType type, const WordSet* boosted, int base, int boost) {
    for (const auto& kw : sqlKeywords(type)) {
        const bool hit = boosted && boosted->contains(kw);
        em.add(kw, CompletionKind::Keyword, {}, {}, em.keyword(kw), hit ? base + boost : base,
               false);
    }
}

void addFunctions(Emitter& em, DatabaseType type, const CompletionCatalog& catalog, int base) {
    for (const auto& fn : sqlFunctions(type))
        em.add(fn, CompletionKind::Function, "function", {}, em.keyword(fn), base, false);
    for (const auto& r : catalog.routines) {
        if (r.kind != RoutineKind::Function)
            continue;
        std::string detail = r.signature.empty() ? r.name : r.signature;
        if (!r.returnType.empty())
            detail += " -> " + r.returnType;
        em.add(r.name, CompletionKind::Function, std::move(detail), {}, em.name(r.name), base + 20);
    }
}

void addRelations(Emitter& em, const CompletionCatalog& catalog, int tableBonus, int viewBonus) {
    auto addList = [&](const std::vector<Table>& list, CompletionKind kind, int bonus) {
        for (const auto& t : list) {
            const bool local = t.schema.empty() || ieq(t.schema, catalog.defaultSchema);
            const std::string rel =
                local ? std::string() : relativeSchema(t.schema, catalog.defaultSchema);
            em.add(t.name, kind, kind == CompletionKind::View ? "view" : "table", t.schema,
                   em.path(rel, t.name), local ? bonus : bonus - 40);
        }
    };
    addList(catalog.tables, CompletionKind::Table, tableBonus);
    addList(catalog.views, CompletionKind::View, viewBonus);
}

std::vector<std::string> allSchemas(const CompletionCatalog& catalog) {
    std::vector<std::string> out = catalog.schemas;
    for (const auto& t : catalog.tables)
        out.push_back(t.schema);
    for (const auto& t : catalog.views)
        out.push_back(t.schema);
    for (const auto& s : catalog.sequences)
        out.push_back(s.schema);
    std::erase_if(out, [](const std::string& s) { return s.empty(); });
    std::ranges::sort(out, iless);
    auto dup = std::ranges::unique(out, ieq);
    out.erase(dup.begin(), dup.end());
    return out;
}

void addSchemas(Emitter& em, const CompletionCatalog& catalog, int bonus) {
    std::unordered_set<std::string> seen;
    for (const auto& s : allSchemas(catalog)) {
        // offer the first segment as typed from the default's level
        const std::string rel = relativeSchema(s, catalog.defaultSchema);
        const auto segs = splitPath(rel);
        if (segs.empty() || !seen.insert(lower(segs[0])).second)
            continue;
        em.add(segs[0], CompletionKind::Schema, "schema", {}, em.name(segs[0]), bonus);
    }
}

// members of `qualifier.`: schemas below it, relations in it
void addSchemaMembers(Emitter& em, const CompletionCatalog& catalog,
                      const std::vector<std::string>& qualifier, int bonus, bool withSequences) {
    auto inSchema = [&](const std::string& schema) {
        return Analyzer::qualifierMatches(schema, qualifier);
    };
    for (const auto& t : catalog.tables) {
        if (inSchema(t.schema))
            em.add(t.name, CompletionKind::Table, "table", t.schema, em.name(t.name), bonus);
    }
    for (const auto& t : catalog.views) {
        if (inSchema(t.schema))
            em.add(t.name, CompletionKind::View, "view", t.schema, em.name(t.name), bonus - 10);
    }
    if (withSequences) {
        for (const auto& s : catalog.sequences) {
            if (inSchema(s.schema))
                em.add(s.name, CompletionKind::Sequence, "sequence", s.schema, em.name(s.name),
                       bonus - 30);
        }
    }
    // next schema segment: `db.` -> dbo, sales
    std::unordered_set<std::string> seen;
    for (const auto& s : allSchemas(catalog)) {
        const auto segs = splitPath(s);
        if (segs.size() <= qualifier.size())
            continue;
        bool match = true;
        for (size_t i = 0; i < qualifier.size(); ++i) {
            if (!ieq(segs[i], qualifier[i])) {
                match = false;
                break;
            }
        }
        const std::string& next = segs[qualifier.size()];
        if (match && seen.insert(lower(next)).second)
            em.add(next, CompletionKind::Schema, "schema", s, em.name(next), bonus - 20);
    }
}

void addColumns(Emitter& em, const std::vector<ColInfo>& cols, const std::string& owner,
                int bonus) {
    for (const auto& c : cols)
        em.add(c.name, CompletionKind::Column, c.type, owner, em.name(c.name), bonus);
}

std::string refDetail(const Analyzer& a, const RelationRef& r) {
    if (r.derived >= 0)
        return "subquery";
    if (r.path.size() == 1 && a.findCte(r.path[0]))
        return "cte";
    std::string out;
    for (const auto& p : r.path) {
        if (!out.empty())
            out += '.';
        out += p;
    }
    return out;
}

} // namespace

// ---------- public api ----------

const std::vector<std::string>& sqlKeywords(DatabaseType type) {
    static const auto all = [] {
        std::array<std::vector<std::string>, kTypeCount> a;
        for (size_t i = 0; i < kTypeCount; ++i)
            a[i] = buildKeywords(static_cast<DatabaseType>(i));
        return a;
    }();
    return all[std::min(static_cast<size_t>(type), kTypeCount - 1)];
}

const std::vector<std::string>& sqlFunctions(DatabaseType type) {
    static const auto all = [] {
        std::array<std::vector<std::string>, kTypeCount> a;
        for (size_t i = 0; i < kTypeCount; ++i)
            a[i] = buildFunctions(static_cast<DatabaseType>(i));
        return a;
    }();
    return all[std::min(static_cast<size_t>(type), kTypeCount - 1)];
}

bool identifierNeedsQuoting(std::string_view name, DatabaseType type) {
    if (name.empty())
        return true;
    if (!isIdentStartByte(static_cast<unsigned char>(name[0])))
        return true;
    const bool foldsLower = type == DatabaseType::POSTGRESQL || type == DatabaseType::REDSHIFT ||
                            type == DatabaseType::CASSANDRA;
    const bool foldsUpper = type == DatabaseType::ORACLE;
    for (const char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '_' || c >= 0x80))
            return true;
        if (foldsLower && std::isupper(c))
            return true;
        if (foldsUpper && std::islower(c))
            return true;
    }
    return inSet(reservedWords(), name);
}

std::string quoteIdentifierIfNeeded(std::string_view name, DatabaseType type) {
    if (!identifierNeedsQuoting(name, type))
        return std::string(name);
    return builderFor(type).quoteIdentifier(std::string(name));
}

std::pair<size_t, size_t> statementRangeAt(std::string_view sql, size_t offset, DatabaseType type) {
    offset = std::min(offset, sql.size());
    const auto tokens = tokenize(sql, dialectFor(type));
    const auto span = statementAt(tokens, offset, sql.size());
    size_t begin = span.begin;
    while (begin < span.end && std::isspace(static_cast<unsigned char>(sql[begin])))
        ++begin;
    size_t end = span.end;
    while (end > begin && std::isspace(static_cast<unsigned char>(sql[end - 1])))
        --end;
    return {begin, end};
}

CompletionResult complete(std::string_view sql, size_t cursor, const CompletionCatalog& catalog,
                          DatabaseType type) {
    cursor = std::min(cursor, sql.size());
    CompletionResult result;
    result.replaceStart = result.replaceEnd = cursor;
    if (!isSqlDialect(type))
        return result;

    const Analyzer a(sql, cursor, catalog, type);

    // inside a string, comment, number or parameter: nothing
    for (const auto& t : a.allTokens()) {
        if (t.begin >= cursor)
            break;
        switch (t.kind) {
        case Tok::Comment:
            if (t.lineComment ? cursor <= t.end : (cursor < t.end || !t.closed))
                return result;
            break;
        case Tok::String:
            if (cursor < t.end || !t.closed)
                return result;
            break;
        case Tok::Number:
        case Tok::Param:
            if (cursor <= t.end)
                return result;
            break;
        default:
            break;
        }
    }

    const auto& toks = a.toks();
    const int n = a.size();

    // the identifier being typed and the token index the completion sits at
    int cur = n;
    bool quoted = false;
    std::string prefix;
    for (int i = 0; i < n; ++i) {
        const auto& t = toks[static_cast<size_t>(i)];
        if (t.begin >= cursor) {
            cur = i;
            break;
        }
        if (t.kind == Tok::Word && cursor <= t.end) {
            cur = i;
            prefix = std::string(sql.substr(t.begin, cursor - t.begin));
            result.replaceStart = t.begin;
            result.replaceEnd = t.end;
            break;
        }
        if (t.kind == Tok::QuotedIdent && (cursor < t.end || !t.closed)) {
            cur = i;
            quoted = true;
            prefix = std::string(sql.substr(t.begin + 1, cursor - t.begin - 1));
            result.replaceStart = t.begin;
            result.replaceEnd = t.end;
            break;
        }
    }

    int chainStart = cur;
    const auto qualifier = a.qualifierBefore(cur, chainStart);
    const int group = a.groupAt(cur);
    const Context ctx = a.detectContext(chainStart);

    Emitter em(prefix, quoted, type);

    if (!qualifier.empty()) {
        // members of alias. / table. / schema.
        if (ctx.kind != Ctx::Relation && ctx.kind != Ctx::AfterRelation) {
            std::vector<ColInfo> cols;
            std::string owner = qualifier.back();
            bool found = false;
            if (qualifier.size() == 1) {
                if (const auto* r = a.findRef(qualifier[0], group)) {
                    cols = a.refColumns(*r);
                    found = true;
                } else if (const auto* c = a.findCte(qualifier[0])) {
                    cols = a.cteColumns(*c, 0);
                    found = true;
                }
            }
            if (!found) {
                if (auto rel = a.findRelation(qualifier); rel.table)
                    cols = Analyzer::tableColumns(*rel.table);
            }
            addColumns(em, cols, owner, 900);
        }
        addSchemaMembers(em, catalog, qualifier, 700, ctx.kind != Ctx::Relation);
        result.items = em.take();
        return result;
    }

    switch (ctx.kind) {
    case Ctx::None:
        break;
    case Ctx::StatementStart:
        addKeywords(em, type, &statementKeywords(), 500, 300);
        break;
    case Ctx::Keyword:
        addKeywords(em, type, nullptr, 500, 0);
        break;
    case Ctx::AfterRelation:
        addKeywords(em, type, &clauseKeywords(), 400, 400);
        break;
    case Ctx::Relation: {
        for (const auto& c : a.ctes())
            em.add(c.name, CompletionKind::Table, "cte", {}, em.name(c.name), 950);
        addRelations(em, catalog, 900, 880);
        addSchemas(em, catalog, 700);
        if (!prefix.empty()) {
            static const WordSet relationKeywords = {"LATERAL", "ONLY",   "SELECT", "IF",
                                                     "NOT",     "EXISTS", "UNNEST"};
            // only words that can start a FROM item, not the whole keyword list
            for (const auto& kw : sqlKeywords(type))
                if (relationKeywords.contains(kw))
                    em.add(kw, CompletionKind::Keyword, {}, {}, em.keyword(kw), 300, false);
        }
        break;
    }
    case Ctx::TargetColumns: {
        std::vector<ColInfo> cols;
        if (ctx.target.size() == 1) {
            if (const auto* c = a.findCte(ctx.target[0]))
                cols = a.cteColumns(*c, 0);
        }
        if (cols.empty()) {
            if (auto rel = a.findRelation(ctx.target); rel.table)
                cols = Analyzer::tableColumns(*rel.table);
        }
        addColumns(em, cols, ctx.target.back(), 900);
        break;
    }
    case Ctx::Column: {
        const auto refs = a.visibleRefs(group);
        std::unordered_set<std::string> named;
        for (const auto* r : refs) {
            if (!named.insert(lower(r->name)).second)
                continue;
            addColumns(em, a.refColumns(*r), r->name, 900);
            em.add(r->name, CompletionKind::Alias, refDetail(a, *r), {}, em.name(r->name), 750);
        }
        addFunctions(em, type, catalog, 600);
        addKeywords(em, type, nullptr, 450, 0);
        if (refs.empty()) {
            // nothing in scope yet (SELECT before FROM, CREATE INDEX ... ON)
            addRelations(em, catalog, 300, 280);
        }
        if (type == DatabaseType::ORACLE) {
            for (const auto& s : catalog.sequences)
                em.add(s.name, CompletionKind::Sequence, "sequence", s.schema, em.name(s.name),
                       200);
        }
        break;
    }
    }

    result.items = em.take();
    return result;
}

std::optional<ResolvedIdentifier> resolveIdentifierAt(std::string_view sql, size_t offset,
                                                      const CompletionCatalog& catalog,
                                                      DatabaseType type) {
    offset = std::min(offset, sql.size());
    if (!isSqlDialect(type))
        return std::nullopt;
    const Analyzer a(sql, offset, catalog, type);
    const auto& toks = a.toks();

    int at = -1;
    for (int i = 0; i < a.size(); ++i) {
        const auto& t = toks[static_cast<size_t>(i)];
        if (!a.isIdent(i))
            continue;
        if (t.begin <= offset && offset < t.end) {
            at = i;
            break;
        }
        if (offset == t.end)
            at = i; // cursor right after the word
    }
    if (at < 0)
        return std::nullopt;

    const auto& tok = toks[static_cast<size_t>(at)];
    ResolvedIdentifier out;
    out.start = tok.begin;
    out.end = tok.end;
    const std::string& name = tok.text;

    int chainStart = at;
    const auto qualifier = a.qualifierBefore(at, chainStart);
    const int group = a.groupOf(at);

    auto setRelation = [&](const Relation& rel) {
        out.kind = rel.isView ? CompletionKind::View : CompletionKind::Table;
        out.table = rel.table;
        out.relation = rel.table->name;
        out.schema = rel.table->schema;
    };

    auto columnIn = [&](const std::vector<ColInfo>& cols,
                        std::string_view col) -> std::optional<ColInfo> {
        for (const auto& c : cols) {
            if (ieq(c.name, col))
                return c;
        }
        return std::nullopt;
    };

    // relation (and its catalog table, when there is one) a ref points to
    auto describeRef = [&](const RelationRef& r) {
        out.alias = r.alias;
        if (r.derived < 0 && !r.function && !(r.path.size() == 1 && a.findCte(r.path[0]))) {
            if (auto rel = a.findRelation(r.path); rel.table) {
                setRelation(rel);
                return;
            }
        }
        out.kind = CompletionKind::Alias;
        out.relation = r.path.empty() ? r.name : r.path.back();
    };

    if (!qualifier.empty()) {
        // qualifier.name: a column of an alias/table, or a relation in a schema
        const RelationRef* r = qualifier.size() == 1 ? a.findRef(qualifier[0], group) : nullptr;
        if (r) {
            if (const auto c = columnIn(a.refColumns(*r), name)) {
                describeRef(*r);
                out.kind = CompletionKind::Column;
                out.column = c->name;
                out.columnInfo = c->column;
                return out;
            }
        }
        if (qualifier.size() == 1) {
            if (const auto* cte = a.findCte(qualifier[0])) {
                if (const auto c = columnIn(a.cteColumns(*cte, 0), name)) {
                    out.kind = CompletionKind::Column;
                    out.relation = cte->name;
                    out.column = c->name;
                    out.columnInfo = c->column;
                    return out;
                }
            }
        }
        if (auto rel = a.findRelation(qualifier); rel.table) {
            for (const auto& c : rel.table->columns) {
                if (ieq(c.name, name)) {
                    setRelation(rel);
                    out.kind = CompletionKind::Column;
                    out.column = c.name;
                    out.columnInfo = &c;
                    return out;
                }
            }
        }
        auto path = qualifier;
        path.push_back(name);
        if (auto rel = a.findRelation(path); rel.table) {
            setRelation(rel);
            return out;
        }
        for (const auto& s : catalog.sequences) {
            if (ieq(s.name, name) && Analyzer::qualifierMatches(s.schema, qualifier)) {
                out.kind = CompletionKind::Sequence;
                out.relation = s.name;
                out.schema = s.schema;
                return out;
            }
        }
        return std::nullopt;
    }

    // a bare name: what it can be depends on where it sits
    const bool isQualifier = a.isPunct(at + 1, '.');
    const Context ctx = a.detectContext(at);

    auto asRef = [&] {
        const auto* r = a.findRef(name, group);
        if (r)
            describeRef(*r);
        return r != nullptr;
    };
    auto asCte = [&] {
        const auto* c = a.findCte(name);
        if (c) {
            out.kind = CompletionKind::Alias;
            out.relation = c->name;
        }
        return c != nullptr;
    };
    auto asRelation = [&] {
        const auto rel = a.findRelation({name});
        if (rel.table)
            setRelation(rel);
        return rel.table != nullptr;
    };
    auto asSchema = [&] {
        for (const auto& s : allSchemas(catalog)) {
            for (const auto& seg : splitPath(s)) {
                if (ieq(seg, name)) {
                    out.kind = CompletionKind::Schema;
                    out.schema = s;
                    return true;
                }
            }
        }
        return false;
    };
    auto asColumn = [&] {
        if (ctx.kind == Ctx::TargetColumns) {
            const auto rel = a.findRelation(ctx.target);
            if (!rel.table)
                return false;
            for (const auto& c : rel.table->columns) {
                if (ieq(c.name, name)) {
                    setRelation(rel);
                    out.kind = CompletionKind::Column;
                    out.column = c.name;
                    out.columnInfo = &c;
                    return true;
                }
            }
            return false;
        }
        for (const auto* r : a.visibleRefs(group)) {
            if (const auto c = columnIn(a.refColumns(*r), name)) {
                describeRef(*r);
                out.kind = CompletionKind::Column;
                out.column = c->name;
                out.columnInfo = c->column;
                return true;
            }
        }
        return false;
    };

    bool found = false;
    if (isQualifier && ctx.kind == Ctx::Relation)
        found = asCte() || asSchema() || asRelation();
    else if (isQualifier)
        found = asRef() || asCte() || asRelation() || asSchema();
    else if (ctx.kind == Ctx::Relation)
        found = asCte() || asRelation() || asSchema();
    else if (ctx.kind == Ctx::AfterRelation)
        found = asRef() || asRelation() || asCte();
    else
        found = asColumn() || asRef() || asCte() || asRelation() || asSchema();
    if (!found)
        return std::nullopt;
    return out;
}

} // namespace dearsql
