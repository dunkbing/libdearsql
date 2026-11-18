// a .xlsx loaded into sqlite: one table per sheet, so every sql feature works on it.
// built only when OpenXLSX is found (DEARSQL_HAS_XLSX)
#include "xlsx_workbook.hpp"
#include <OpenXLSX.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <sqlite3.h>
#include <variant>

namespace dearsql::xlsx {

namespace {

    // shortcut: OpenXLSX holds the sheet as a dom and is slow per cell: measured 7.5 s and
    // 1.1 GB for a 9.8 MB file (200k x 10). past 20 MB a workbook is refused; stream the sheet
    // xml (pugixml over miniz) instead once real workbooks need more
    constexpr std::uintmax_t kMaxFileBytes = 20ull * 1024 * 1024;

    // a cell as read: nothing, a number (date when its format says so), text or a boolean
    struct Date {
        double serial;
    };
    using Cell = std::variant<std::monostate, int64_t, double, Date, bool, std::string>;

    // excel's built-in date and time formats
    bool builtinDateFormat(uint32_t id) {
        return (id >= 14 && id <= 22) || (id >= 45 && id <= 47);
    }

    // a custom format is a date when it has date or time letters outside quotes and brackets
    bool customDateFormat(const std::string& code) {
        bool quoted = false;
        int bracket = 0;
        for (size_t i = 0; i < code.size(); ++i) {
            const char c = code[i];
            if (c == '"')
                quoted = !quoted;
            else if (quoted)
                continue;
            else if (c == '\\')
                ++i; // an escaped literal
            else if (c == '[')
                ++bracket;
            else if (c == ']')
                bracket = std::max(0, bracket - 1);
            else if (bracket == 0 && std::string_view("dmyhsDMYHS").find(c) != std::string_view::npos)
                return true;
        }
        return false;
    }

    // inverse of days_from_civil (howard hinnant)
    void civilFromDays(long long z, int& y, unsigned& m, unsigned& d) {
        z += 719468;
        const long long era = (z >= 0 ? z : z - 146096) / 146097;
        const unsigned doe = static_cast<unsigned>(z - era * 146097);
        const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        const unsigned mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y = static_cast<int>(yoe + era * 400 + (m <= 2));
    }

    // excel serial to "yyyy-mm-dd" or "yyyy-mm-dd hh:mm:ss".
    // shortcut: assumes the 1900 date system; a workbook saved with date1904 (old mac excel)
    // reads 4 years early. read workbookPr/@date1904 when such files turn up
    std::string isoDate(double serial) {
        // serial 0 is 1899-12-30, which is 25569 days before 1970-01-01
        long long days = static_cast<long long>(std::floor(serial));
        long long secs = std::llround((serial - static_cast<double>(days)) * 86400.0);
        if (secs >= 86400) {
            ++days;
            secs -= 86400;
        }
        int y = 0;
        unsigned m = 0, d = 0;
        civilFromDays(days - 25569, y, m, d);
        if (secs == 0)
            return std::format("{:04}-{:02}-{:02}", y, m, d);
        return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", y, m, d, secs / 3600,
                           secs / 60 % 60, secs % 60);
    }

    // a column letter: 1 -> A, 27 -> AA
    std::string columnLetters(uint32_t n) {
        std::string s;
        for (; n > 0; n = (n - 1) / 26)
            s.insert(s.begin(), static_cast<char>('A' + (n - 1) % 26));
        return s;
    }

    std::string quoteIdent(const std::string& name) {
        std::string q = "\"";
        for (const char c : name)
            q += c == '"' ? std::string("\"\"") : std::string(1, c);
        return q + "\"";
    }

    struct Sheet {
        std::string name;
        std::vector<std::vector<Cell>> rows; // dense: missing cells are monostate
        size_t width = 0;
    };

    Sheet readSheet(OpenXLSX::XLDocument& doc, const std::string& name,
                    std::map<uint32_t, bool>& dateFormats) {
        Sheet sheet{.name = name};
        auto ws = doc.workbook().worksheet(name);
        const auto isDate = [&](OpenXLSX::XLStyleIndex xf) {
            const auto it = dateFormats.find(xf);
            if (it != dateFormats.end())
                return it->second;
            bool date = false;
            try {
                const uint32_t id = doc.styles().cellFormats()[xf].numberFormatId();
                date = builtinDateFormat(id) ||
                       (id >= 164 &&
                        customDateFormat(doc.styles().numberFormats().numberFormatById(id).formatCode()));
            } catch (const std::exception&) {
            }
            return dateFormats[xf] = date;
        };
        uint32_t expectRow = 1;
        for (auto& row : ws.rows()) {
            // rows the file leaves out are empty rows
            for (; expectRow < row.rowNumber(); ++expectRow)
                sheet.rows.emplace_back();
            ++expectRow;
            auto& out = sheet.rows.emplace_back();
            for (auto& cell : row.cells()) {
                const uint16_t col = cell.cellReference().column();
                OpenXLSX::XLCellValue v = cell.value();
                Cell c;
                switch (v.type()) {
                case OpenXLSX::XLValueType::Boolean:
                    c = v.get<bool>();
                    break;
                case OpenXLSX::XLValueType::Integer:
                    if (isDate(cell.cellFormat()))
                        c = Date{static_cast<double>(v.get<int64_t>())};
                    else
                        c = v.get<int64_t>();
                    break;
                case OpenXLSX::XLValueType::Float:
                    if (isDate(cell.cellFormat()))
                        c = Date{v.get<double>()};
                    else
                        c = v.get<double>();
                    break;
                case OpenXLSX::XLValueType::String:
                    c = v.get<std::string>();
                    break;
                case OpenXLSX::XLValueType::Error:
                    c = v.getString(); // "#DIV/0!" and friends, as excel shows them
                    break;
                default:
                    continue;
                }
                if (out.size() < col)
                    out.resize(col);
                out[col - 1] = std::move(c);
            }
            sheet.width = std::max(sheet.width, out.size());
        }
        // trailing empty rows add nothing
        while (!sheet.rows.empty() &&
               std::ranges::all_of(sheet.rows.back(), [](const Cell& c) {
                   return std::holds_alternative<std::monostate>(c);
               }))
            sheet.rows.pop_back();
        return sheet;
    }

    // the sql type a column's cells fit, most specific first
    enum class Kind { Empty, Integer, Real, Date, Boolean, Text };

    Kind kindOf(const Cell& c) {
        if (std::holds_alternative<int64_t>(c))
            return Kind::Integer;
        if (const auto* d = std::get_if<double>(&c))
            return std::trunc(*d) == *d && std::abs(*d) < 9007199254740992.0 ? Kind::Integer
                                                                            : Kind::Real;
        if (std::holds_alternative<Date>(c))
            return Kind::Date;
        if (std::holds_alternative<bool>(c))
            return Kind::Boolean;
        if (std::holds_alternative<std::string>(c))
            return Kind::Text;
        return Kind::Empty;
    }

    Kind merge(Kind a, Kind b) {
        if (a == Kind::Empty)
            return b;
        if (b == Kind::Empty || a == b)
            return a;
        if ((a == Kind::Integer && b == Kind::Real) || (a == Kind::Real && b == Kind::Integer))
            return Kind::Real;
        return Kind::Text;
    }

    const char* sqlType(Kind k) {
        switch (k) {
        case Kind::Integer:
            return "INTEGER";
        case Kind::Real:
            return "REAL";
        case Kind::Date:
            return "DATETIME";
        case Kind::Boolean:
            return "BOOLEAN";
        default:
            return "TEXT";
        }
    }

    std::string textOf(const Cell& c) {
        if (const auto* s = std::get_if<std::string>(&c))
            return *s;
        if (const auto* i = std::get_if<int64_t>(&c))
            return std::to_string(*i);
        if (const auto* d = std::get_if<double>(&c)) {
            if (std::trunc(*d) == *d && std::abs(*d) < 9007199254740992.0)
                return std::to_string(static_cast<int64_t>(*d));
            return std::format("{}", *d);
        }
        if (const auto* d = std::get_if<Date>(&c))
            return isoDate(d->serial);
        if (const auto* b = std::get_if<bool>(&c))
            return *b ? "TRUE" : "FALSE";
        return "";
    }

    void exec(sqlite3* db, const std::string& sql) {
        char* err = nullptr;
        if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::string msg = err ? err : "sqlite error";
            sqlite3_free(err);
            throw std::runtime_error(msg);
        }
    }

    void loadSheet(sqlite3* db, const Sheet& sheet) {
        const size_t width = std::max<size_t>(sheet.width, 1);
        // a first row of distinct-enough text is the header; anything else is data
        size_t first = 0;
        std::vector<std::string> names;
        const bool header = !sheet.rows.empty() && sheet.rows[0].size() == sheet.width &&
                            std::ranges::all_of(sheet.rows[0], [](const Cell& c) {
                                const auto* s = std::get_if<std::string>(&c);
                                return s && !s->empty();
                            });
        std::set<std::string> taken;
        for (size_t c = 0; c < width; ++c) {
            std::string name = header ? std::get<std::string>(sheet.rows[0][c])
                                      : columnLetters(static_cast<uint32_t>(c + 1));
            std::string lowered = name;
            std::ranges::transform(lowered, lowered.begin(), ::tolower);
            for (int n = 2; taken.contains(lowered); ++n) {
                lowered = std::format("{}_{}", name, n);
                std::ranges::transform(lowered, lowered.begin(), ::tolower);
                if (!taken.contains(lowered))
                    name = std::format("{}_{}", name, n);
            }
            taken.insert(lowered);
            names.push_back(name);
        }
        if (header)
            first = 1;

        std::vector<Kind> kinds(width, Kind::Empty);
        for (size_t r = first; r < sheet.rows.size(); ++r)
            for (size_t c = 0; c < sheet.rows[r].size(); ++c)
                kinds[c] = merge(kinds[c], kindOf(sheet.rows[r][c]));

        std::string create = "CREATE TABLE " + quoteIdent(sheet.name) + " (";
        std::string insert = "INSERT INTO " + quoteIdent(sheet.name) + " VALUES (";
        for (size_t c = 0; c < width; ++c) {
            create += (c ? ", " : "") + quoteIdent(names[c]) + " " + sqlType(kinds[c]);
            insert += c ? ", ?" : "?";
        }
        exec(db, create + ")");

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, (insert + ")").c_str(), -1, &stmt, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
        for (size_t r = first; r < sheet.rows.size(); ++r) {
            const auto& row = sheet.rows[r];
            for (size_t c = 0; c < width; ++c) {
                const int i = static_cast<int>(c + 1);
                const Cell empty;
                const Cell& cell = c < row.size() ? row[c] : empty;
                const Kind k = kindOf(cell);
                if (k == Kind::Empty)
                    sqlite3_bind_null(stmt, i);
                else if (kinds[c] == Kind::Integer || kinds[c] == Kind::Boolean)
                    sqlite3_bind_int64(stmt, i,
                                       std::holds_alternative<bool>(cell)
                                           ? std::get<bool>(cell)
                                           : (std::holds_alternative<int64_t>(cell)
                                                  ? std::get<int64_t>(cell)
                                                  : static_cast<int64_t>(std::get<double>(cell))));
                else if (kinds[c] == Kind::Real)
                    sqlite3_bind_double(stmt, i,
                                        std::holds_alternative<int64_t>(cell)
                                            ? static_cast<double>(std::get<int64_t>(cell))
                                            : std::get<double>(cell));
                else {
                    const std::string text = textOf(cell);
                    sqlite3_bind_text(stmt, i, text.c_str(), static_cast<int>(text.size()),
                                      SQLITE_TRANSIENT);
                }
            }
            if (sqlite3_step(stmt) != SQLITE_DONE)
                throw std::runtime_error(std::format("{} row {}: {}", sheet.name, r + 1,
                                                     sqlite3_errmsg(db)));
            sqlite3_reset(stmt);
        }
    }

} // namespace

Status loadWorkbook(sqlite3* db, const std::string& path) {
    try {
        std::error_code ec;
        if (const auto size = std::filesystem::file_size(path, ec); !ec && size > kMaxFileBytes)
            return {false, std::format("{} is {} MB; workbooks over {} MB are not opened yet",
                                       std::filesystem::path(path).filename().string(),
                                       size / (1024 * 1024), kMaxFileBytes / (1024 * 1024))};
        OpenXLSX::XLDocument doc;
        doc.open(path);
        std::map<uint32_t, bool> dateFormats;
        exec(db, "BEGIN");
        for (const auto& name : doc.workbook().worksheetNames())
            loadSheet(db, readSheet(doc, name, dateFormats));
        exec(db, "COMMIT");
        doc.close();
        return {true, ""};
    } catch (const std::exception& e) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        return {false, std::format("Could not read the workbook: {}", e.what())};
    }
}

} // namespace dearsql::xlsx
