#pragma once

#include "dearsql/connection_info.hpp"
#include "dearsql/types.hpp"

#include <string>
#include <vector>

// Schema diff: what to run on one database to give it another's tables, columns,
// indexes and foreign keys. Pure: the host loads both sides (describeTable) and decides
// what to show and what to run; nothing here touches a connection. Views are left out.
namespace dearsql {

    struct SchemaChange {
        // also the order to run them in: creates, alters, drops last
        enum class Kind {
            CreateTable,
            AddColumn,
            AlterColumn,
            AddIndex,
            AddForeignKey,
            DropForeignKey,
            DropIndex,
            DropColumn,
            DropTable,
        };
        Kind kind{};
        std::string table;   // as it is named on the side being changed
        std::string message; // one line: what differs
        std::string sql;     // statements for the side being changed, ';'-terminated, one per
                             // line; empty when the engine cannot do it (the message says why)
    };

    // changes that make `from` look like `to`, sorted by kind. type: from's engine, whose
    // dialect the statements use; schema: where new tables go (from's schema, may be empty).
    // indexes match on columns and uniqueness, not name, so a renamed index is no change;
    // primary keys are compared as part of the columns
    [[nodiscard]] std::vector<SchemaChange> diffSchemas(const std::vector<Table>& from,
                                                        const std::vector<Table>& to,
                                                        DatabaseType type,
                                                        const std::string& schema = "");

    // true when both speak the same SQL dialect (postgres family, mysql family, or the same
    // engine), so their catalogs and type names can be compared
    [[nodiscard]] bool sameDialect(DatabaseType a, DatabaseType b);

    // DROP INDEX in the engine's form: MySQL and SQL Server name the table (DROP INDEX i ON t).
    // names are used as given (quoted or not, qualified or not)
    [[nodiscard]] std::string dropIndexSql(DatabaseType type, const std::string& index,
                                           const std::string& table);

} // namespace dearsql
