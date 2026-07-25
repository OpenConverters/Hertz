#pragma once
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace Hertz {

// Harmonic-comb detection: estimate a converter's switching frequency from a
// measured EMI spectrum and label which peaks its harmonics explain. Mirror of
// the Python reference (src/hertz/comb.py) — same constants, same vectors.
// "No comb found" is a legitimate result, reported explicitly — never a guess.

inline constexpr double COMB_PROMINENCE_THRESHOLD_DB = 8.0;
inline constexpr double COMB_BASELINE_HALF_WIDTH = 1.3;
inline constexpr double COMB_CLUSTER_TOLERANCE = 0.02;
inline constexpr int COMB_MIN_MATCHED_HARMONICS = 3;
inline constexpr double COMB_CONFIDENCE_THRESHOLD = 0.5;
inline constexpr int COMB_MAX_COVERAGE_HARMONICS = 20;

struct MatchedHarmonic {
    int order;
    double frequencyHz;
    double levelDbuv;
};

struct CombResult {
    bool found = false;
    double fSwHz = 0.0;
    double confidence = 0.0;
    double coverage = 0.0;
    std::vector<MatchedHarmonic> harmonics;
    std::vector<std::pair<double, double>> residualPeaks;  // (frequency, level)
};

namespace detail {

inline std::vector<double> comb_baseline(const std::vector<double>& freqs,
                                         const std::vector<double>& levels) {
    std::vector<double> baseline(freqs.size());
    std::vector<double> window;
    size_t lo = 0, hi = 0;
    for (size_t i = 0; i < freqs.size(); ++i) {
        while (freqs[lo] < freqs[i] / COMB_BASELINE_HALF_WIDTH) ++lo;
        while (hi < freqs.size() && freqs[hi] <= freqs[i] * COMB_BASELINE_HALF_WIDTH) ++hi;
        window.assign(levels.begin() + lo, levels.begin() + hi);
        auto mid = window.begin() + window.size() / 2;
        std::nth_element(window.begin(), mid, window.end());
        double median = *mid;
        if (window.size() % 2 == 0) {
            double below = *std::max_element(window.begin(), mid);
            median = (median + below) / 2.0;
        }
        baseline[i] = median;
    }
    return baseline;
}

struct CombPeaks {
    std::vector<size_t> indices;
    std::vector<double> prominence;
};

inline CombPeaks comb_find_peaks(const std::vector<double>& freqs,
                                 const std::vector<double>& levels) {
    CombPeaks result;
    auto baseline = comb_baseline(freqs, levels);
    result.prominence.resize(levels.size());
    for (size_t i = 0; i < levels.size(); ++i) {
        result.prominence[i] = levels[i] - baseline[i];
    }
    std::vector<size_t> raw;
    for (size_t i = 0; i < levels.size(); ++i) {
        size_t lo = i >= 2 ? i - 2 : 0;
        size_t hi = std::min(levels.size(), i + 3);
        if (result.prominence[i] > COMB_PROMINENCE_THRESHOLD_DB &&
            levels[i] >= *std::max_element(levels.begin() + lo, levels.begin() + hi)) {
            raw.push_back(i);
        }
    }
    for (size_t i : raw) {
        if (!result.indices.empty() && freqs[i] < freqs[result.indices.back()] * 1.01) {
            if (levels[i] > levels[result.indices.back()]) {
                result.indices.back() = i;
            }
        } else {
            result.indices.push_back(i);
        }
    }
    return result;
}

inline double comb_bin_spacing(const std::vector<double>& freqs, double f) {
    auto it = std::lower_bound(freqs.begin(), freqs.end(), f);
    size_t index = std::clamp<size_t>(it - freqs.begin(), 1, freqs.size() - 1);
    return freqs[index] - freqs[index - 1];
}

struct CombMatch {
    std::map<int, size_t> matched;  // order -> peak index
    double total = 0.0;
    double coverage = 0.0;
};

inline CombMatch comb_match(const std::vector<double>& freqs, const CombPeaks& peaks,
                            double candidate, double fMax) {
    CombMatch result;
    for (size_t i : peaks.indices) {
        double f = freqs[i];
        int order = static_cast<int>(std::llround(f / candidate));
        if (order < 1) continue;
        double tolerance = std::min(std::max(2.0 * comb_bin_spacing(freqs, f), 0.005 * f),
                                    0.3 * candidate);
        if (std::abs(f - order * candidate) <= tolerance) {
            auto it = result.matched.find(order);
            if (it == result.matched.end() || peaks.prominence[i] > peaks.prominence[it->second]) {
                result.matched[order] = i;
            }
        }
    }
    int possible = std::max(1, std::min(static_cast<int>(fMax / candidate),
                                        COMB_MAX_COVERAGE_HARMONICS));
    int inRange = 0;
    double score = 0.0;
    for (const auto& [order, index] : result.matched) {
        if (order <= possible) ++inRange;
        score += peaks.prominence[index];
    }
    result.coverage = static_cast<double>(inRange) / possible;
    result.total = score * std::sqrt(result.coverage);
    return result;
}

}  // namespace detail

inline CombResult detect_comb(const std::vector<double>& freqs, const std::vector<double>& levels,
                              double fSwMinHz = 20e3, double fSwMaxHz = 5e6) {
    if (freqs.size() != levels.size()) {
        throw std::invalid_argument("frequency and level arrays differ in length");
    }
    if (freqs.size() < 16) {
        throw std::invalid_argument("spectrum too short for comb detection (need >= 16 points)");
    }
    for (size_t i = 1; i < freqs.size(); ++i) {
        if (freqs[i] <= freqs[i - 1]) {
            throw std::invalid_argument("frequencies must be strictly increasing");
        }
    }
    if (fSwMinHz <= 0.0 || fSwMinHz >= fSwMaxHz) {
        throw std::invalid_argument("bad f_sw search range");
    }

    CombResult result;
    auto peaks = detail::comb_find_peaks(freqs, levels);
    auto residuals = [&](const std::map<int, size_t>* matched) {
        for (size_t i : peaks.indices) {
            bool inMatched = false;
            if (matched != nullptr) {
                for (const auto& [order, index] : *matched) {
                    if (index == i) { inMatched = true; break; }
                }
            }
            if (!inMatched) result.residualPeaks.emplace_back(freqs[i], levels[i]);
        }
    };
    if (peaks.indices.size() < COMB_MIN_MATCHED_HARMONICS) {
        residuals(nullptr);
        return result;
    }

    // candidate fundamentals: clustered adjacent-peak gaps, plus the lowest peak
    std::vector<std::pair<double, int>> clusters;  // (sum, count)
    for (size_t k = 1; k < peaks.indices.size(); ++k) {
        double gap = freqs[peaks.indices[k]] - freqs[peaks.indices[k - 1]];
        if (gap < fSwMinHz || gap > fSwMaxHz) continue;
        bool placed = false;
        for (auto& [sum, count] : clusters) {
            if (std::abs(gap - sum / count) <= COMB_CLUSTER_TOLERANCE * gap) {
                sum += gap;
                ++count;
                placed = true;
                break;
            }
        }
        if (!placed) clusters.emplace_back(gap, 1);
    }
    std::vector<double> candidates;
    for (const auto& [sum, count] : clusters) candidates.push_back(sum / count);
    double lowestPeak = freqs[peaks.indices.front()];
    if (lowestPeak >= fSwMinHz && lowestPeak <= fSwMaxHz) candidates.push_back(lowestPeak);
    if (candidates.empty()) {
        residuals(nullptr);
        return result;
    }

    double bestScore = -1.0;
    double bestCandidate = 0.0;
    detail::CombMatch best;
    for (double candidate : candidates) {
        auto match = detail::comb_match(freqs, peaks, candidate, freqs.back());
        if (match.total > bestScore) {
            bestScore = match.total;
            bestCandidate = candidate;
            best = match;
        }
    }

    // prominence-weighted least squares through the origin: f ≈ n·f_sw
    double numerator = 0.0, denominator = 0.0;
    for (const auto& [order, index] : best.matched) {
        double w = peaks.prominence[index];
        numerator += w * order * freqs[index];
        denominator += w * order * order;
    }
    double fSw = denominator > 0.0 ? numerator / denominator : bestCandidate;

    auto refined = detail::comb_match(freqs, peaks, fSw, freqs.back());
    double totalProminence = 0.0, matchedProminence = 0.0;
    for (size_t i : peaks.indices) totalProminence += peaks.prominence[i];
    for (const auto& [order, index] : refined.matched) matchedProminence += peaks.prominence[index];
    result.confidence = totalProminence > 0.0 ? matchedProminence / totalProminence : 0.0;
    result.coverage = refined.coverage;
    result.found = result.confidence >= COMB_CONFIDENCE_THRESHOLD &&
                   refined.matched.size() >= COMB_MIN_MATCHED_HARMONICS;
    if (result.found) {
        result.fSwHz = fSw;
        for (const auto& [order, index] : refined.matched) {
            result.harmonics.push_back({order, freqs[index], levels[index]});
        }
    }
    residuals(&refined.matched);  // matches the reference: matched peaks are never listed as residual
    return result;
}

}  // namespace Hertz
