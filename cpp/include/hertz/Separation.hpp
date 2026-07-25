#pragma once
#include <stdexcept>
#include <utility>
#include <vector>

namespace Hertz {

// Exact CM/DM separation needs the two LISN line voltages as synchronized
// time-domain samples or complex spectra: V_CM = (V_L + V_N)/2,
// V_DM = (V_L - V_N)/2 (ANP015 eqs. 1-2). It is NOT possible from two
// magnitude-only spectra — the relative phase is gone — which is why no such
// convenience overload exists.
template <typename T>
inline std::pair<std::vector<T>, std::vector<T>> separate(const std::vector<T>& vLine,
                                                          const std::vector<T>& vNeutral) {
    if (vLine.size() != vNeutral.size()) {
        throw std::invalid_argument("line and neutral records differ in length");
    }
    if (vLine.empty()) {
        throw std::invalid_argument("empty input");
    }
    std::vector<T> commonMode(vLine.size());
    std::vector<T> differentialMode(vLine.size());
    for (size_t i = 0; i < vLine.size(); ++i) {
        commonMode[i] = (vLine[i] + vNeutral[i]) / 2.0;
        differentialMode[i] = (vLine[i] - vNeutral[i]) / 2.0;
    }
    return {std::move(commonMode), std::move(differentialMode)};
}

}  // namespace Hertz
