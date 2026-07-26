#pragma once
#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Hertz {

// Limit VALUES are facts of the referenced standards; texts are not reproduced.
// Frequencies outside every segment throw OutsideCoverage: CISPR 25 defines
// limits only inside protected broadcast bands, so "no limit here" is real
// information, never a zero.

class OutsideCoverage : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum class Detector { PEAK, QUASI_PEAK, AVERAGE };

struct LimitSegment {
    double fStartHz;
    double fStopHz;
    double levelStartDbuv;
    double levelStopDbuv;

    LimitSegment(double fStart, double fStop, double levelStart, double levelStop)
        : fStartHz(fStart), fStopHz(fStop), levelStartDbuv(levelStart), levelStopDbuv(levelStop) {
        if (!(0.0 < fStart && fStart < fStop)) {
            throw std::invalid_argument("invalid limit segment frequencies");
        }
    }

    double level(double fHz) const {
        double span = std::log10(fStopHz) - std::log10(fStartHz);
        double fraction = (std::log10(fHz) - std::log10(fStartHz)) / span;
        return levelStartDbuv + fraction * (levelStopDbuv - levelStartDbuv);
    }
};

class LimitLine {
  public:
    LimitLine(std::string name, Detector detector, std::vector<LimitSegment> segments)
        : _name(std::move(name)), _detector(detector), _segments(std::move(segments)) {
        if (_segments.empty()) {
            throw std::invalid_argument("limit line needs at least one segment");
        }
        std::sort(_segments.begin(), _segments.end(),
                  [](const auto& a, const auto& b) { return a.fStartHz < b.fStartHz; });
        for (size_t i = 1; i < _segments.size(); ++i) {
            if (_segments[i].fStartHz < _segments[i - 1].fStopHz * (1.0 - 1e-9)) {
                throw std::invalid_argument("overlapping segments in limit line " + _name);
            }
        }
    }

    const std::string& name() const { return _name; }
    Detector detector() const { return _detector; }
    const std::vector<LimitSegment>& segments() const { return _segments; }

    bool covers(double fHz) const {
        for (const auto& segment : _segments) {
            if (segment.fStartHz <= fHz && fHz <= segment.fStopHz) {
                return true;
            }
        }
        return false;
    }

    double level(double fHz) const {
        // At a shared segment boundary the LOWER limit applies (the CISPR
        // transition rule) — never "whichever segment happens to come first".
        // Class A at exactly 500 kHz is 73 dBuV, not 79.
        std::optional<double> best;
        for (const auto& segment : _segments) {
            if (segment.fStartHz <= fHz && fHz <= segment.fStopHz) {
                double value = segment.level(fHz);
                if (!best.has_value() || value < *best) {
                    best = value;
                }
            }
        }
        if (!best.has_value()) {
            throw OutsideCoverage(std::to_string(fHz) + " Hz is outside limit line " + _name);
        }
        return *best;
    }

    // limit - measured: positive means passing.
    double margin(double fHz, double measuredDbuv) const { return level(fHz) - measuredDbuv; }

  private:
    std::string _name;
    Detector _detector;
    std::vector<LimitSegment> _segments;
};

// Parts of a limit line's regulated spans the scan NEVER REACHED — everything
// of each segment outside [fLoHz, fHiHz] (the scan's own extent). A verdict
// naming a standard implies the standard's whole range: a 150 kHz-30 MHz sweep
// judged against CISPR 25 has not measured the 68-108 MHz FM band at all, and
// saying "PASS, >=10 dB in hand" without that qualification is a false clean
// bill. Contiguous regions across touching segments are merged for reporting.
inline std::vector<std::pair<double, double>> unswept_regions(const LimitLine& line,
                                                              double fLoHz, double fHiHz) {
    std::vector<std::pair<double, double>> regions;
    for (const auto& segment : line.segments()) {
        if (segment.fStartHz < fLoHz) {
            regions.emplace_back(segment.fStartHz, std::min(segment.fStopHz, fLoHz));
        }
        if (segment.fStopHz > fHiHz) {
            regions.emplace_back(std::max(segment.fStartHz, fHiHz), segment.fStopHz);
        }
    }
    std::sort(regions.begin(), regions.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto& region : regions) {
        if (!merged.empty() && region.first <= merged.back().second * (1.0 + 1e-9)) {
            merged.back().second = std::max(merged.back().second, region.second);
        } else {
            merged.push_back(region);
        }
    }
    return merged;
}

// Drawable polyline of a limit line: one run PER SEGMENT with the exact band
// edges always emitted — decade sampling alone leaves a band narrower than the
// grid spacing (CISPR 25 SW is 5.9-6.2 MHz) with a single invisible point, and
// insets every drawn band edge by up to half a grid step. Runs are clipped to
// [fMinHz, fMaxHz]; interpolated spans get pointsPerDecade points between the
// exact endpoints. Adjacent segments stay separate runs, so a limit step is
// drawn as a step, not a slant.
inline std::vector<std::vector<std::pair<double, double>>> limit_polyline_runs(
    const LimitLine& line, double fMinHz, double fMaxHz, int pointsPerDecade) {
    if (!(0.0 < fMinHz && fMinHz < fMaxHz) || pointsPerDecade < 2) {
        throw std::invalid_argument("bad polyline range");
    }
    std::vector<std::vector<std::pair<double, double>>> runs;
    for (const auto& segment : line.segments()) {
        double f0 = std::max(segment.fStartHz, fMinHz);
        double f1 = std::min(segment.fStopHz, fMaxHz);
        if (f0 > f1) {
            continue;
        }
        std::vector<std::pair<double, double>> run;
        int steps = std::max(1, static_cast<int>(std::ceil((std::log10(f1) - std::log10(f0)) *
                                                           pointsPerDecade)));
        for (int i = 0; i <= steps; ++i) {
            double f = i == 0 ? f0
                     : i == steps ? f1
                     : std::pow(10.0, std::log10(f0) + (std::log10(f1) - std::log10(f0)) * i / steps);
            run.emplace_back(f, segment.level(f));
        }
        runs.push_back(std::move(run));
    }
    return runs;
}

// CISPR 32 / EN 55032, AC mains conducted, 150 kHz - 30 MHz.
inline const LimitLine& cispr32_class_b_mains_qp() {
    static const LimitLine line("CISPR 32 Class B mains (QP)", Detector::QUASI_PEAK,
                                {{150e3, 500e3, 66.0, 56.0}, {500e3, 5e6, 56.0, 56.0}, {5e6, 30e6, 60.0, 60.0}});
    return line;
}
inline const LimitLine& cispr32_class_b_mains_avg() {
    static const LimitLine line("CISPR 32 Class B mains (AVG)", Detector::AVERAGE,
                                {{150e3, 500e3, 56.0, 46.0}, {500e3, 5e6, 46.0, 46.0}, {5e6, 30e6, 50.0, 50.0}});
    return line;
}
inline const LimitLine& cispr32_class_a_mains_qp() {
    static const LimitLine line("CISPR 32 Class A mains (QP)", Detector::QUASI_PEAK,
                                {{150e3, 500e3, 79.0, 79.0}, {500e3, 30e6, 73.0, 73.0}});
    return line;
}
inline const LimitLine& cispr32_class_a_mains_avg() {
    static const LimitLine line("CISPR 32 Class A mains (AVG)", Detector::AVERAGE,
                                {{150e3, 500e3, 66.0, 66.0}, {500e3, 30e6, 60.0, 60.0}});
    return line;
}

// CISPR 25 conducted, voltage method, supply lines: limits exist only inside the
// protected broadcast bands. Class 5 QP baseline per CISPR 25:2016 Table 5;
// peak = QP + 13 dB, average = QP - 7 dB; each class step relaxes 10 dB.
struct Cispr25Band {
    const char* name;
    double fStartHz;
    double fStopHz;
    double class5QuasiPeakDbuv;
    double classStepDb;  // per-class step of THIS band (Table 4 is band-dependent)
};

// CISPR 25 Table 4, conducted voltage method. The class-to-class step is NOT a
// flat 10 dB: 10 dB in LW, 8 dB in MW, 6 dB from SW up — deriving class 3/4
// as class 5 + 10/20 dB was up to 8 dB too permissive. The 68-87 MHz mobile-
// services band and the 76-108 MHz FM band carry identical limits at every
// class and detector, so their union is one segment.
inline constexpr std::array<Cispr25Band, 6> CISPR25_BANDS_CLASS5{{
    {"LW", 150e3, 300e3, 57.0, 10.0},
    {"MW", 530e3, 1.8e6, 41.0, 8.0},
    {"SW", 5.9e6, 6.2e6, 40.0, 6.0},
    {"CB", 26e6, 28e6, 31.0, 6.0},
    {"VHF 30-54", 30e6, 54e6, 31.0, 6.0},
    {"VHF/FM 68-108", 68e6, 108e6, 25.0, 6.0},
}};

inline LimitLine cispr25_conducted_voltage(int emissionClass, Detector detector) {
    if (emissionClass < 1 || emissionClass > 5) {
        throw std::invalid_argument("CISPR 25 class must be 1..5");
    }
    double detectorOffset;
    switch (detector) {
        case Detector::PEAK: detectorOffset = 13.0; break;
        case Detector::QUASI_PEAK: detectorOffset = 0.0; break;
        case Detector::AVERAGE: detectorOffset = -7.0; break;
        default: throw std::invalid_argument("unknown detector");
    }
    std::vector<LimitSegment> segments;
    for (const auto& band : CISPR25_BANDS_CLASS5) {
        double offset = detectorOffset + band.classStepDb * (5 - emissionClass);
        segments.emplace_back(band.fStartHz, band.fStopHz, band.class5QuasiPeakDbuv + offset,
                              band.class5QuasiPeakDbuv + offset);
    }
    return LimitLine("CISPR 25 Class " + std::to_string(emissionClass) + " conducted", detector,
                     std::move(segments));
}

}  // namespace Hertz
