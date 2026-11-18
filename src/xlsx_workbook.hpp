#pragma once

#include "dearsql/database.hpp"
#include <string>

struct sqlite3;

namespace dearsql::xlsx {
// every sheet of the workbook at `path` as a table in `db` (header row -> column names,
// types inferred over each whole column, dates as iso text); one transaction
Status loadWorkbook(sqlite3* db, const std::string& path);
} // namespace dearsql::xlsx
