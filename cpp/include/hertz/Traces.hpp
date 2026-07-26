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
// 60 dB mistakes happen, so ambiguity throws.

class TraceFormatError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct SpectrumTrace {
    std::vector<double> frequenciesHz;
    std::vector<double> levelsDbuv;
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

inline std::optional<std::string> find_one_unit(const std::vector<std::string>& tokens,
                                                const std::vector<std::string>& units,
                                                const std::string& what,
                                                bool overrideAvailable = false) {
    std::vector<std::string> hits;
    for (const auto& unit : units) {
        if (std::find(tokens.begin(), tokens.end(), unit) != tokens.end()) {
            hits.push_back(unit);
        }
    }
    if (hits.size() > 1) {
        // An analyzer preamble can legitimately mention several units
        // ("Start 150 kHz / Stop 30 MHz"). A single stated unit always beats
        // the override (R-1), but an AMBIGUOUS header decides nothing — the
        // user's explicit override may resolve it; without one, fail loudly.
        if (overrideAvailable) {
            return std::nullopt;
        }
        throw TraceFormatError("conflicting " + what + " units in header");
    }
    if (hits.size() == 1) {
        return hits[0];
    }
    return std::nullopt;
}

}  // namespace detail

inline SpectrumTrace parse_spectrum_csv(const std::string& content,
                                        std::optional<std::string> freqUnit = std::nullopt,
                                        std::optional<std::string> levelUnit = std::nullopt,
                                        double z0Ohm = 50.0) {
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

    char delimiter = '\0';
    size_t bestColumns = 1;
    for (char candidate : {',', ';', '\t'}) {
        size_t columns = 1;
        for (size_t i = 0; i < lines.size() && i < 20; ++i) {
            columns = std::max(columns, detail::split(lines[i], candidate).size());
        }
        if (columns > bestColumns) {
            bestColumns = columns;
            delimiter = candidate;
        }
    }
    if (delimiter == '\0') {
        throw TraceFormatError("no recognizable column delimiter (',', ';' or tab)");
    }

    std::string headerText;
    std::vector<std::pair<double, double>> rows;
    for (const auto& line : lines) {
        if (line.empty()) {
            continue;
        }
        std::vector<double> numeric;
        for (const auto& token : detail::split(line, delimiter)) {
            if (auto value = detail::parse_number(token)) {
                numeric.push_back(*value);
            }
        }
        if (numeric.size() >= 2) {
            rows.emplace_back(numeric[0], numeric[1]);
        } else if (rows.empty()) {
            headerText += " " + line;
        }
    }
    if (rows.size() < 2) {
        throw TraceFormatError("fewer than two data rows found");
    }

    auto headerTokens = detail::alnum_tokens(detail::normalize(headerText));
    auto fileFreqUnit = detail::find_one_unit(headerTokens, {"ghz", "mhz", "khz", "hz"}, "frequency",
                                              freqUnit.has_value());
    auto fileLevelUnit = detail::find_one_unit(headerTokens, {"dbuv", "dbm"}, "level",
                                               levelUnit.has_value());

    // R-1 contract, enforced HERE and not only in callers: a unit the header
    // STATES always wins; a passed override only fills an axis the header left
    // silent (or an ambiguous header that find_one_unit declined to decide).
    std::string freq = detail::normalize(fileFreqUnit.value_or(freqUnit.value_or("")));
    std::string level = detail::normalize(fileLevelUnit.value_or(levelUnit.value_or("")));

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
    if (level != "dbuv" && level != "dbm") {
        throw TraceFormatError("level unit not stated in the file header — select it in the unit control");
    }

    std::vector<size_t> order(rows.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&rows](size_t a, size_t b) { return rows[a].first < rows[b].first; });

    SpectrumTrace trace;
    trace.frequenciesHz.reserve(rows.size());
    trace.levelsDbuv.reserve(rows.size());
    for (size_t index : order) {
        trace.frequenciesHz.push_back(rows[index].first * freqScale);
        double value = rows[index].second;
        trace.levelsDbuv.push_back(level == "dbm" ? dbuv_from_dbm(value, z0Ohm) : value);
    }
    return trace;
}

}  // namespace Hertz
