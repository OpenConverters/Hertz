#pragma once
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "hertz/Utils.hpp"

namespace Hertz {

// Spectrum-analyzer trace ingestion from an in-memory string — the WASM-native
// design: the host (browser File API, Python open()) reads the file and passes
// its content. Units are taken from the header row; if the header does not
// state them they MUST be passed explicitly — guessing units on EMI data is how
// 60 dB mistakes happen, so ambiguity throws. The same zero-guessing rule
// applies to multi-trace exports (Peak/QP/Average side by side, the normal
// receiver format): with more than two numeric columns the caller MUST say
// which column to judge — silently taking the second one turned a failing QP
// scan into a passing Average read.

class TraceFormatError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SpectrumTrace {
    std::vector<double> frequenciesHz;
    std::vector<double> levelsDbuv;   // in levelUnit's domain: dBuV (dBm converted), or dBuA passthrough
    std::string levelUnit;            // "dbuv" | "dbua" — what the numbers ARE
};

namespace detail {

// Lowercase copy with both UTF-8 micro signs (µ U+00B5, μ U+03BC) folded to 'u'.
inline std::string normalize(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xB5) {
            out += 'u';
            ++i;
        } else if (c == 0xCE && i + 1 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) == 0xBC) {
            out += 'u';
            ++i;
        } else {
            out += static_cast<char>(std::tolower(c));
        }
    }
    return out;
}

inline std::vector<std::string> alnum_tokens(const std::string& normalized) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : normalized) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += c;
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

inline std::vector<std::string> split(const std::string& line, char delimiter) {
    std::vector<std::string> tokens;
    std::string current;
    for (char c : line) {
        if (c == delimiter) {
            tokens.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    tokens.push_back(current);
    return tokens;
}

inline std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

inline std::optional<double> parse_number(const std::string& token) {
    const char* begin = token.c_str();
    char* end = nullptr;
    double value = std::strtod(begin, &end);
    if (end == begin) {
        return std::nullopt;
    }
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) {
            return std::nullopt;
        }
        ++end;
    }
    return value;
}

// Numeric field parse with European-locale support: when the column delimiter
// is NOT the comma, a token like "72,5" is a decimal-comma number (German
// analyzer exports are ';'-delimited with ',' decimals) — retry with '.'.
inline std::optional<double> parse_field(const std::string& token, char delimiter) {
    if (auto value = parse_number(token)) {
        return value;
    }
    if (delimiter != ',' && token.find('.') == std::string::npos &&
        std::count(token.begin(), token.end(), ',') == 1) {
        std::string dotted = token;
        dotted[dotted.find(',')] = '.';
        return parse_number(dotted);
    }
    return std::nullopt;
}

struct CsvScan {
    char delimiter = '\0';
    std::vector<std::string> headerLines;    // pre-data lines, in order
    std::vector<std::vector<double>> rows;   // EVERY numeric field per data row
    std::vector<size_t> firstRowTokenIndex;  // token position of each numeric column
    size_t numericColumns = 0;               // max numeric fields over all rows
};

inline CsvScan scan_csv(const std::string& content) {
    if (content.empty()) {
        throw TraceFormatError("empty trace content");
    }
    std::string body = content;
    if (body.size() >= 3 && static_cast<unsigned char>(body[0]) == 0xEF) {
        body.erase(0, 3);  // UTF-8 BOM
    }

    std::vector<std::string> lines;
    std::string current;
    for (char c : body) {
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c != '\r') {
            current += c;
        }
    }
    if (!current.empty()) {
        lines.push_back(current);
    }

    CsvScan scan;
    // Preference order, not raw column count: a European ';' export carries
    // decimal commas, and counting columns would elect ',' and shred "0,15"
    // into a 0 Hz row. ';' and tab can only be delimiters on purpose.
    for (char candidate : {';', '\t', ','}) {
        size_t columns = 1;
        for (size_t i = 0; i < lines.size() && i < 20; ++i) {
            columns = std::max(columns, split(lines[i], candidate).size());
        }
        if (columns >= 2) {
            scan.delimiter = candidate;
            break;
        }
    }
    if (scan.delimiter == '\0') {
        throw TraceFormatError("no recognizable column delimiter (',', ';' or tab)");
    }

    for (const auto& line : lines) {
        if (line.empty()) {
            continue;
        }
        auto tokens = split(line, scan.delimiter);
        std::vector<double> numeric;
        std::vector<size_t> positions;
        for (size_t t = 0; t < tokens.size(); ++t) {
            if (auto value = parse_field(tokens[t], scan.delimiter)) {
                numeric.push_back(*value);
                positions.push_back(t);
            }
        }
        if (numeric.size() >= 2) {
            if (scan.rows.empty()) {
                scan.firstRowTokenIndex = positions;
            }
            scan.numericColumns = std::max(scan.numericColumns, numeric.size());
            scan.rows.push_back(std::move(numeric));
        } else if (scan.rows.empty()) {
            scan.headerLines.push_back(line);
        }
    }
    if (scan.rows.size() < 2) {
        throw TraceFormatError("fewer than two data rows found");
    }
    return scan;
}

// Header cell text for 1-based numeric column k, or "" when the header does
// not reach that column. Cells align with the numeric columns through the
// token positions of the first data row.
inline std::string column_header_cell(const CsvScan& scan, size_t column) {
    if (scan.headerLines.empty() || column == 0 || column > scan.firstRowTokenIndex.size()) {
        return "";
    }
    auto cells = split(scan.headerLines.back(), scan.delimiter);
    size_t tokenIndex = scan.firstRowTokenIndex[column - 1];
    return tokenIndex < cells.size() ? trim(cells[tokenIndex]) : "";
}

// 1-based numeric column carrying the frequency axis. Column 1 unless the
// header names exactly one OTHER column as frequency (a frequency unit token
// or "freq" in its cell) — the "No.,Frequency (MHz),Level" index-first export
// shape is common, and reading the index as megahertz fabricates the whole
// axis. Two frequency-looking columns are ambiguous and throw.
inline size_t frequency_column(const CsvScan& scan) {
    std::vector<size_t> candidates;
    for (size_t k = 1; k <= scan.numericColumns; ++k) {
        std::string cell = normalize(column_header_cell(scan, k));
        if (cell.empty()) {
            continue;
        }
        auto tokens = alnum_tokens(cell);
        bool unitHit = false;
        for (const char* unit : {"ghz", "mhz", "khz", "hz"}) {
            if (std::find(tokens.begin(), tokens.end(), unit) != tokens.end()) {
                unitHit = true;
            }
        }
        if (unitHit || cell.find("freq") != std::string::npos) {
            candidates.push_back(k);
        }
    }
    if (candidates.size() > 1) {
        throw TraceFormatError(
            "several columns look like the frequency axis — clean the export so exactly one "
            "column names a frequency");
    }
    return candidates.empty() ? 1 : candidates[0];
}

// Unit search from the most specific context outward: the column's own header
// cell, then the column-header line, then the analyzer preamble above it. The
// first tier that mentions any candidate decides — so "RBW 9 kHz" in the
// preamble can never contradict a column that states "Frequency (Hz)". A
// single hit is a STATED unit (beats any override, R-1); several distinct
// hits in one tier decide nothing — the override may resolve it, else throw.
inline std::optional<std::string> find_unit_tiered(const std::vector<std::string>& tiers,
                                                   const std::vector<std::string>& units,
                                                   const std::string& what,
                                                   bool overrideAvailable) {
    for (const auto& tier : tiers) {
        auto tokens = alnum_tokens(normalize(tier));
        std::vector<std::string> hits;
        for (const auto& unit : units) {
            if (std::find(tokens.begin(), tokens.end(), unit) != tokens.end()) {
                hits.push_back(unit);
            }
        }
        if (hits.size() == 1) {
            return hits[0];
        }
        if (hits.size() > 1) {
            if (overrideAvailable) {
                return std::nullopt;
            }
            throw TraceFormatError("conflicting " + what + " units in header");
        }
    }
    return std::nullopt;
}

}  // namespace detail

// Numeric-column inventory of a trace file, for callers that must offer a
// level-column choice on multi-trace exports. names[k] labels 1-based numeric
// column k+1 from the header row ("" when the header has no cell for it).
struct SpectrumCsvColumns {
    size_t count = 0;
    size_t frequencyColumn = 1;
    std::vector<std::string> names;
};

inline SpectrumCsvColumns spectrum_csv_columns(const std::string& content) {
    auto scan = detail::scan_csv(content);
    SpectrumCsvColumns columns;
    columns.count = scan.numericColumns;
    columns.frequencyColumn = detail::frequency_column(scan);
    for (size_t k = 1; k <= scan.numericColumns; ++k) {
        columns.names.push_back(detail::column_header_cell(scan, k));
    }
    return columns;
}

inline SpectrumTrace parse_spectrum_csv(const std::string& content,
                                        std::optional<std::string> freqUnit = std::nullopt,
                                        std::optional<std::string> levelUnit = std::nullopt,
                                        double z0Ohm = 50.0,
                                        std::optional<int> levelColumn = std::nullopt) {
    auto scan = detail::scan_csv(content);
    size_t freqColumn = detail::frequency_column(scan);

    size_t level;
    if (levelColumn.has_value()) {
        level = static_cast<size_t>(*levelColumn);
        if (level < 1 || level > scan.numericColumns) {
            throw TraceFormatError("level column " + std::to_string(level) + " out of range — the file has " +
                                   std::to_string(scan.numericColumns) + " numeric columns");
        }
        if (level == freqColumn) {
            throw TraceFormatError("level column " + std::to_string(level) +
                                   " is the frequency column — pick a level trace");
        }
    } else if (scan.numericColumns == 2) {
        level = freqColumn == 1 ? 2 : 1;
    } else {
        std::string message =
            "multiple level columns: the file carries " + std::to_string(scan.numericColumns) +
            " numeric columns (a multi-trace export) — say which one to judge:";
        for (size_t k = 1; k <= scan.numericColumns; ++k) {
            if (k == freqColumn) {
                continue;
            }
            std::string cell = detail::column_header_cell(scan, k);
            message += " " + std::to_string(k) + ": \"" +
                       (cell.empty() ? "column " + std::to_string(k) : cell) + "\"";
        }
        throw TraceFormatError(message);
    }

    std::string columnLine = scan.headerLines.empty() ? "" : scan.headerLines.back();
    std::string preamble;
    for (size_t i = 0; i + 1 < scan.headerLines.size(); ++i) {
        preamble += " " + scan.headerLines[i];
    }
    auto fileFreqUnit = detail::find_unit_tiered(
        {detail::column_header_cell(scan, freqColumn), columnLine, preamble},
        {"ghz", "mhz", "khz", "hz"}, "frequency", freqUnit.has_value());
    auto fileLevelUnit = detail::find_unit_tiered(
        {detail::column_header_cell(scan, level), columnLine, preamble},
        {"dbuv", "dbm", "dbua"}, "level", levelUnit.has_value());

    // R-1 contract, enforced HERE and not only in callers: a unit the header
    // STATES always wins; a passed override only fills an axis the header left
    // silent (or an ambiguous header that find_unit_tiered declined to decide).
    std::string freq = detail::normalize(fileFreqUnit.value_or(freqUnit.value_or("")));
    std::string levelUnitName = detail::normalize(fileLevelUnit.value_or(levelUnit.value_or("")));

    double freqScale;
    if (freq == "hz") {
        freqScale = 1.0;
    } else if (freq == "khz") {
        freqScale = 1e3;
    } else if (freq == "mhz") {
        freqScale = 1e6;
    } else if (freq == "ghz") {
        freqScale = 1e9;
    } else {
        throw TraceFormatError("frequency unit not stated in the file header — select it in the unit control");
    }
    if (levelUnitName != "dbuv" && levelUnitName != "dbm" && levelUnitName != "dbua") {
        throw TraceFormatError("level unit not stated in the file header — select it in the unit control");
    }

    size_t needed = std::max(freqColumn, level);
    for (size_t r = 0; r < scan.rows.size(); ++r) {
        const auto& row = scan.rows[r];
        if (row.size() < needed) {
            throw TraceFormatError("data row " + std::to_string(r + 1) + " has only " +
                                   std::to_string(row.size()) + " numeric columns — needed " +
                                   std::to_string(needed));
        }
        double f = row[freqColumn - 1] * freqScale;
        if (!std::isfinite(f) || !std::isfinite(row[level - 1])) {
            throw TraceFormatError("non-finite value in data row " + std::to_string(r + 1) +
                                   " — clean the export before judging it");
        }
        if (f <= 0.0) {
            throw TraceFormatError("non-positive frequency (" + std::to_string(f) +
                                   " Hz) in data row " + std::to_string(r + 1) +
                                   " — remove DC/index rows, and check the delimiter/decimal convention");
        }
    }

    std::vector<size_t> order(scan.rows.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&scan, freqColumn](size_t a, size_t b) {
        return scan.rows[a][freqColumn - 1] < scan.rows[b][freqColumn - 1];
    });

    SpectrumTrace trace;
    trace.levelUnit = levelUnitName == "dbua" ? "dbua" : "dbuv";
    trace.frequenciesHz.reserve(scan.rows.size());
    trace.levelsDbuv.reserve(scan.rows.size());
    for (size_t index : order) {
        trace.frequenciesHz.push_back(scan.rows[index][freqColumn - 1] * freqScale);
        double value = scan.rows[index][level - 1];
        trace.levelsDbuv.push_back(levelUnitName == "dbm" ? dbuv_from_dbm(value, z0Ohm) : value);
    }
    return trace;
}

}  // namespace Hertz
