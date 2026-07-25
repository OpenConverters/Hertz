#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Hertz {

inline constexpr std::array<double, 6> E6{1.0, 1.5, 2.2, 3.3, 4.7, 6.8};
inline constexpr std::array<double, 12> E12{1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2};
inline constexpr std::array<double, 24> E24{1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                            3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1};

inline constexpr double RELATIVE_TOLERANCE = 1e-9;

inline double dbuv_from_vrms(double vRms) {
    if (vRms < 0.0) {
        throw std::invalid_argument("RMS voltage must be non-negative");
    }
    if (vRms == 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(vRms / 1e-6);
}

inline double vrms_from_dbuv(double dbuv) {
    return 1e-6 * std::pow(10.0, dbuv / 20.0);
}

inline double dbuv_from_dbm(double dbm, double z0Ohm = 50.0) {
    if (z0Ohm <= 0.0) {
        throw std::invalid_argument("reference impedance must be positive");
    }
    return dbm + 10.0 * std::log10(z0Ohm) + 90.0;
}

inline double round_up_to(double value, std::vector<double> candidates) {
    if (value <= 0.0) {
        throw std::invalid_argument("value must be positive");
    }
    std::sort(candidates.begin(), candidates.end());
    for (double candidate : candidates) {
        if (candidate >= value * (1.0 - RELATIVE_TOLERANCE)) {
            return candidate;
        }
    }
    throw std::invalid_argument("no candidate >= requested value");
}

inline double round_down_to(double value, std::vector<double> candidates) {
    if (value <= 0.0) {
        throw std::invalid_argument("value must be positive");
    }
    std::sort(candidates.begin(), candidates.end());
    double chosen = std::numeric_limits<double>::quiet_NaN();
    for (double candidate : candidates) {
        if (candidate <= value * (1.0 + RELATIVE_TOLERANCE)) {
            chosen = candidate;
        }
    }
    if (std::isnan(chosen)) {
        throw std::invalid_argument("no candidate <= requested value");
    }
    return chosen;
}

template <typename Series>
inline double round_up_to_series(double value, const Series& series) {
    int exponent = static_cast<int>(std::floor(std::log10(value)));
    std::vector<double> candidates;
    for (int e : {exponent, exponent + 1}) {
        for (double s : series) {
            candidates.push_back(s * std::pow(10.0, e));
        }
    }
    return round_up_to(value, candidates);
}

template <typename Series>
inline double round_down_to_series(double value, const Series& series) {
    int exponent = static_cast<int>(std::floor(std::log10(value)));
    std::vector<double> candidates;
    for (int e : {exponent - 1, exponent}) {
        for (double s : series) {
            candidates.push_back(s * std::pow(10.0, e));
        }
    }
    return round_down_to(value, candidates);
}

}  // namespace Hertz
