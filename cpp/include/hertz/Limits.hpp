#pragma once
#include <algorithm>
#include <array>
#include <cctype>
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
// Interior-hole criteria. A consecutive-sample gap is a HOLE (not sampling
// density) when EITHER it exceeds an absolute factor no real sweep uses
// (0.35 dec = 2.24x), OR it is grossly out of line with the scan's LOCAL
// spacing — > 6x the 90th-percentile of the neighboring gaps on EACH
// available side, in BOTH log-frequency and linear frequency. Local, not
// global: a coarse 120 kHz segment above 30 MHz must not veto detection of a
// gutted band at 200 kHz (round 14); per-side, so a legitimate density
// CHANGE (dense sweep + coarse sweep spliced) never flags its own
// transition; both domains, so log sweeps and linear exports are each judged
// against their own uniformity. A hole is only EMITTED when its clipped
// regulated width reaches 10x the band's RBW — a width gate, NOT a log-ratio
// floor: a frequency-ratio floor made an identical 1.85 MHz dropout
// reportable at 15.1 MHz and invisible at 15.2 MHz (round 15), while grid-
// alignment slivers (the 20 kHz a 60 kHz step leaves at a band edge) are a
// couple of RBW bins and stay suppressed. Independently, a gap that swallows
// >= 90% of a regulated segment's width is a hole regardless of any density
// argument (two edge samples do not measure a band).
inline constexpr double UNSWEPT_GAP_DECADES = 0.35;
inline constexpr double UNSWEPT_RELATIVE_FACTOR = 6.0;
inline constexpr size_t UNSWEPT_LOCAL_WINDOW = 12;
inline constexpr double UNSWEPT_SEGMENT_SWALLOW = 0.9;
inline constexpr double UNSWEPT_MIN_HOLE_RBW = 10.0;

// CISPR 16-1-1 resolution bandwidth of the band containing f — an unswept
// region narrower than the receiver's own RBW cannot even be resolved as
// unmeasured (it also swallows the float dust and half-RBW-below-stop
// slivers ordinary exports produce at band edges).
inline double cispr_rbw_hz(double fHz) {
    if (fHz < 150e3) return 200.0;
    if (fHz < 30e6) return 9e3;
    return 120e3;
}

inline std::vector<std::pair<double, double>> merge_regions(
    std::vector<std::pair<double, double>> regions) {
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
    regions = merge_regions(std::move(regions));
    std::erase_if(regions, [](const std::pair<double, double>& r) {
        return r.second - r.first < cispr_rbw_hz(r.first);
    });
    return regions;
}

// Sampling-aware variant: extent regions PLUS interior holes — regulated
// sub-spans the sweep jumped over (spliced band-by-band acquisitions, a lost
// segment, a retune gap). Reported holes are clipped to the limit's segments,
// so the unregulated space between CISPR 25 bands is never flagged.
inline std::vector<std::pair<double, double>> unswept_regions(const LimitLine& line,
                                                              std::vector<double> freqsHz) {
    if (freqsHz.empty()) {
        throw std::invalid_argument("unswept_regions needs at least one sample frequency");
    }
    std::sort(freqsHz.begin(), freqsHz.end());
    auto regions = unswept_regions(line, freqsHz.front(), freqsHz.back());

    std::vector<double> logGaps;
    std::vector<double> linGaps;
    for (size_t i = 1; i < freqsHz.size(); ++i) {
        logGaps.push_back(freqsHz[i - 1] > 0.0 ? std::log10(freqsHz[i] / freqsHz[i - 1]) : 0.0);
        linGaps.push_back(freqsHz[i] - freqsHz[i - 1]);
    }
    // NOTE: on a 12-element window the (n*9)/10 index is the 11th of 12 —
    // effectively the local near-MAX, not a median. That bias is toward
    // silence, which is the intended direction for a density reference.
    auto percentile90 = [](std::vector<double> values) {
        std::sort(values.begin(), values.end());
        return values[std::min(values.size() - 1, (values.size() * 9) / 10)];
    };
    // outlier against ONE side's window; an empty side cannot veto
    auto outlierVsSide = [&](size_t gapIndex, double gapLog, double gapLin, bool preceding) {
        size_t begin = preceding ? (gapIndex >= UNSWEPT_LOCAL_WINDOW ? gapIndex - UNSWEPT_LOCAL_WINDOW : 0)
                                 : gapIndex + 1;
        size_t end = preceding ? gapIndex : std::min(logGaps.size(), gapIndex + 1 + UNSWEPT_LOCAL_WINDOW);
        if (begin >= end) {
            return true;
        }
        std::vector<double> logWindow(logGaps.begin() + begin, logGaps.begin() + end);
        std::vector<double> linWindow(linGaps.begin() + begin, linGaps.begin() + end);
        return gapLog > UNSWEPT_RELATIVE_FACTOR * percentile90(std::move(logWindow)) &&
               gapLin > UNSWEPT_RELATIVE_FACTOR * percentile90(std::move(linWindow));
    };
    for (size_t i = 1; i < freqsHz.size(); ++i) {
        double fa = freqsHz[i - 1];
        double fb = freqsHz[i];
        if (fa <= 0.0) {
            continue;
        }
        double gapLog = std::log10(fb / fa);
        bool hole = gapLog > UNSWEPT_GAP_DECADES ||
                    (outlierVsSide(i - 1, gapLog, fb - fa, true) &&
                     outlierVsSide(i - 1, gapLog, fb - fa, false));
        for (const auto& segment : line.segments()) {
            double f0 = std::max(fa, segment.fStartHz);
            double f1 = std::min(fb, segment.fStopHz);
            if (f0 >= f1) {
                continue;
            }
            bool swallowsSegment =
                f1 - f0 >= UNSWEPT_SEGMENT_SWALLOW * (segment.fStopHz - segment.fStartHz);
            if ((hole && f1 - f0 >= UNSWEPT_MIN_HOLE_RBW * cispr_rbw_hz(f0)) || swallowsSegment) {
                regions.emplace_back(f0, f1);
            }
        }
    }

    // A regulated segment the scan's extent overlaps but that contains NO
    // sample at all is unswept regardless of any gap ratio — CISPR 25's
    // narrow bands fit whole inside a modest-looking jump (25 -> 30 MHz
    // skips 26-28 MHz entirely at a factor of only 1.2).
    for (const auto& segment : line.segments()) {
        double f0 = std::max(segment.fStartHz, freqsHz.front());
        double f1 = std::min(segment.fStopHz, freqsHz.back());
        if (f0 >= f1) {
            continue;
        }
        auto it = std::lower_bound(freqsHz.begin(), freqsHz.end(), segment.fStartHz);
        if (it == freqsHz.end() || *it > segment.fStopHz) {
            regions.emplace_back(f0, f1);
        }
    }
    regions = merge_regions(std::move(regions));
    std::erase_if(regions, [](const std::pair<double, double>& r) {
        return r.second - r.first < cispr_rbw_hz(r.first);
    });
    return regions;
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

// CISPR 32 / EN 55032 radiated disturbance limits (Tables A.2 / A.3):
// quasi-peak, 120 kHz RBW, 30-1000 MHz, breakpoint at 230 MHz. Values in
// dBuV/m at the STANDARD's measurement distances: 10 m, and the 3 m
// alternative (+10 dB inverse-distance).
inline LimitLine cispr32_radiated(char emissionClass, double distanceM) {
    double base;
    if (emissionClass == 'B' || emissionClass == 'b') {
        base = 30.0;
    } else if (emissionClass == 'A' || emissionClass == 'a') {
        base = 40.0;
    } else {
        throw std::invalid_argument("CISPR 32 radiated class must be A or B");
    }
    if (distanceM == 3.0) {
        base += 10.0;
    } else if (distanceM != 10.0) {
        throw std::invalid_argument("CISPR 32 radiated limits are specified at 10 m or 3 m");
    }
    return LimitLine("CISPR 32 Class " + std::string(1, std::toupper(emissionClass)) +
                         " radiated @ " + (distanceM == 3.0 ? "3 m" : "10 m") + " (QP, dBuV/m)",
                     Detector::QUASI_PEAK,
                     {{30e6, 230e6, base, base}, {230e6, 1000e6, base + 7.0, base + 7.0}});
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
