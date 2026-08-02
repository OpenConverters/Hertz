#pragma once
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace Hertz {

// Two-port ABCD chains for in-circuit insertion loss. The asymptotic
// 40·n·log10(f/f0) sizing rule says what to buy; these curves say what the
// bought filter actually does between real terminations — including the
// CISPR 17 "approximate worst case" 0.1 Ω/100 Ω profiles, which expose the
// termination sensitivity that 50 Ω/50 Ω data sheets hide.

using Complex = std::complex<double>;

struct Abcd {
    Complex a{1.0, 0.0}, b{0.0, 0.0}, c{0.0, 0.0}, d{1.0, 0.0};
};

inline Abcd series_impedance(Complex z) {
    return {{1.0, 0.0}, z, {0.0, 0.0}, {1.0, 0.0}};
}

inline Abcd shunt_admittance(Complex y) {
    return {{1.0, 0.0}, {0.0, 0.0}, y, {1.0, 0.0}};
}

inline Abcd cascade(const Abcd& first, const Abcd& second) {
    return {first.a * second.a + first.b * second.c, first.a * second.b + first.b * second.d,
            first.c * second.a + first.d * second.c, first.c * second.b + first.d * second.d};
}

// IL = 20·log10 |(A·Zl + B + C·Zs·Zl + D·Zs) / (Zs + Zl)| — the ratio of the
// load voltage without and with the network between source and load.
inline double insertion_loss_db(const Abcd& network, Complex zSource, Complex zLoad) {
    Complex numerator = network.a * zLoad + network.b + network.c * zSource * zLoad +
                        network.d * zSource;
    return 20.0 * std::log10(std::abs(numerator / (zSource + zLoad)));
}

// One LC low-pass stage per ANP015: series inductor from the noise source,
// shunt capacitor at the measurement side; identical stages cascaded.
// capEslH / capEsrOhm: series parasitics of the SHUNT capacitor branch (#290)
// — above the cap's self-resonance 1/(2*pi*sqrt(ESL*C)) the branch turns
// inductive and the real filter stops improving. Defaults of 0 keep the ideal
// element.
inline Abcd lc_filter_abcd(double fHz, double inductanceH, double capacitanceF, int stages,
                           double capEslH = 0.0, double capEsrOhm = 0.0) {
    if (fHz <= 0.0 || inductanceH <= 0.0 || capacitanceF <= 0.0) {
        throw std::invalid_argument("frequency, inductance and capacitance must be positive");
    }
    if (stages < 1 || stages > 4) {
        throw std::invalid_argument("stages must be 1..4");
    }
    if (capEslH < 0.0 || capEsrOhm < 0.0) {
        throw std::invalid_argument("capacitor parasitics must be non-negative");
    }
    double w = 2.0 * std::numbers::pi * fHz;
    Complex shuntZ(capEsrOhm, w * capEslH - 1.0 / (w * capacitanceF));
    Abcd stage = cascade(series_impedance(Complex(0.0, w * inductanceH)),
                         shunt_admittance(Complex(1.0, 0.0) / shuntZ));
    Abcd network;
    for (int s = 0; s < stages; ++s) {
        network = cascade(network, stage);
    }
    return network;
}

// CISPR 17 approximate worst case: evaluate 0.1 Ω→100 Ω and 100 Ω→0.1 Ω and
// keep the smaller insertion loss.
inline double insertion_loss_worst_case_db(const Abcd& network) {
    return std::min(insertion_loss_db(network, {0.1, 0.0}, {100.0, 0.0}),
                    insertion_loss_db(network, {100.0, 0.0}, {0.1, 0.0}));
}

struct InsertionLossCurves {
    std::vector<double> frequenciesHz;
    std::vector<double> standardDb;   // symmetric reference terminations
    std::vector<double> worstCaseDb;  // CISPR 17 0.1/100 Ω profiles
};

// referenceImpedanceOhm: 25 Ω for the CM path (two 50 Ω LISN arms in parallel),
// 100 Ω for the DM path (the two arms in series), 50 Ω for data-sheet style.
// chokeEpcF > 0 bounds the CM curve by the choke's inter-winding (EPC) capacitance:
// above self-resonance that parasitic shunts the CM inductance, so real CM IL
// plateaus instead of reaching the ideal LC notch's unphysical hundreds of dB. Pass
// it for a CM curve; leave 0 for DM. Kept consistent with FilterBlock's block CM
// ceiling (LayoutParasitics::cm_layout_il_db, same 3 pF nominal).
inline InsertionLossCurves insertion_loss_curves(double inductanceH, double capacitanceF,
                                                 int stages, double referenceImpedanceOhm,
                                                 double fMinHz, double fMaxHz,
                                                 int pointsPerDecade = 40,
                                                 double capEslH = 0.0, double capEsrOhm = 0.0,
                                                 double chokeEpcF = 0.0) {
    if (!(0.0 < fMinHz && fMinHz < fMaxHz) || pointsPerDecade < 2) {
        throw std::invalid_argument("bad frequency range");
    }
    if (referenceImpedanceOhm <= 0.0) {
        throw std::invalid_argument("reference impedance must be positive");
    }
    InsertionLossCurves curves;
    double logMin = std::log10(fMinHz);
    double logMax = std::log10(fMaxHz);
    // Guard steps>=1: a span narrower than one 1/pointsPerDecade step truncates to
    // steps=0, and the loop below then evaluates i/steps = 0/0 = NaN. Matches the
    // floor already applied in Limits.hpp::limit_polyline_runs. (max(1,...) only,
    // no ceil, so point counts for every non-degenerate span are unchanged.)
    int steps = std::max(1, static_cast<int>((logMax - logMin) * pointsPerDecade));
    Complex reference(referenceImpedanceOhm, 0.0);
    for (int i = 0; i <= steps; ++i) {
        double f = std::pow(10.0, logMin + (logMax - logMin) * i / steps);
        Abcd network = lc_filter_abcd(f, inductanceH, capacitanceF, stages, capEslH, capEsrOhm);
        curves.frequenciesHz.push_back(f);
        double std_ = insertion_loss_db(network, reference, reference);
        double worst_ = insertion_loss_worst_case_db(network);
        if (chokeEpcF > 0.0) {
            // cap by the EPC bypass: |T| = |T_filter| + |T_epc|, T_epc = w·Cw·zSys
            constexpr double TWO_PI = 6.283185307179586;
            const double zSys = 2.0 * referenceImpedanceOhm;
            const double tEpc = TWO_PI * f * chokeEpcF * zSys;
            std_ = -20.0 * std::log10(std::pow(10.0, -std_ / 20.0) + tEpc);
            worst_ = -20.0 * std::log10(std::pow(10.0, -worst_ / 20.0) + tEpc);
        }
        curves.standardDb.push_back(std_);
        curves.worstCaseDb.push_back(worst_);
    }
    return curves;
}

// Insertion loss using a MEASURED complex series impedance per stage (the
// bound part's impedance curve) against the shunt capacitor — evaluated AT the
// measured frequencies only: no interpolation, no extrapolation, no invented
// points. Outputs align with the input frequency vector.
inline InsertionLossCurves tabulated_series_il_curves(const std::vector<double>& freqsHz,
                                                      const std::vector<double>& zRealOhm,
                                                      const std::vector<double>& zImagOhm,
                                                      double cShuntF, int stages,
                                                      double referenceImpedanceOhm) {
    if (freqsHz.size() != zRealOhm.size() || freqsHz.size() != zImagOhm.size()) {
        throw std::invalid_argument("frequency and impedance arrays differ in length");
    }
    if (freqsHz.size() < 2) {
        throw std::invalid_argument("need >= 2 measured points");
    }
    for (size_t i = 1; i < freqsHz.size(); ++i) {
        if (freqsHz[i] <= freqsHz[i - 1]) {
            throw std::invalid_argument("frequencies must be strictly increasing");
        }
    }
    if (cShuntF <= 0.0 || referenceImpedanceOhm <= 0.0) {
        throw std::invalid_argument("shunt capacitance and reference impedance must be positive");
    }
    if (stages < 1 || stages > 4) {
        throw std::invalid_argument("stages must be 1..4");
    }
    InsertionLossCurves curves;
    Complex reference(referenceImpedanceOhm, 0.0);
    for (size_t i = 0; i < freqsHz.size(); ++i) {
        double w = 2.0 * std::numbers::pi * freqsHz[i];
        Abcd stage = cascade(series_impedance(Complex(zRealOhm[i], zImagOhm[i])),
                             shunt_admittance(Complex(0.0, w * cShuntF)));
        Abcd network;
        for (int s = 0; s < stages; ++s) {
            network = cascade(network, stage);
        }
        curves.frequenciesHz.push_back(freqsHz[i]);
        curves.standardDb.push_back(insertion_loss_db(network, reference, reference));
        curves.worstCaseDb.push_back(insertion_loss_worst_case_db(network));
    }
    return curves;
}

}  // namespace Hertz
