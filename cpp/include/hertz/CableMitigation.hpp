#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "hertz/Limits.hpp"    // OutsideCoverage
#include "hertz/Network.hpp"

namespace Hertz {

// Cable-level common-mode mitigation — the measure the RADIATED band
// (30 MHz-1 GHz) actually responds to, where a mains EMI filter's choke has long
// since self-resonated. A clip-on ferrite / cable common-mode choke is, to first
// order, a pure SERIES common-mode impedance Z(f) on the offending cable; its
// insertion loss against the CM loop impedance is the two-port result for a lone
// series element (Network::insertion_loss_db):
//
//     IL(f) = 20*log10 |(Zs + Z(f) + Zl) / (Zs + Zl)|
//
// Threading the cable through the core N times multiplies the impedance by N^2
// (the winding rule). select_cable_mitigation walks a candidate list of REAL
// parts (each a datasheet |Z|(f) curve) and returns the least one meeting a
// CM-attenuation target (e.g. radiated_cm_attenuation_target_db) across the
// flagged band, or the best near-miss with its shortfall — never a silent miss.
//
// Two honest constraints from the physics, not papered over:
//  * The CM loop impedance is genuinely uncertain (it IS the +-20 dB in the
//    radiated screen), so it is an explicit argument with no hidden default.
//  * A ferrite's impedance is COMPLEX. When the datasheet/catalog carries phase
//    (phaseRad below), it is used as Z = |Z|*e^{j*phase}; when only |Z| is known
//    (phaseRad empty) it is treated as RESISTIVE — true in the loss band where
//    beads are used (|Z| ~ R), conservative below it (small |Z| anyway).

struct FerritePart {
    std::string name;
    std::vector<double> frequenciesHz;  // datasheet |Z| curve, strictly increasing
    std::vector<double> zOhm;           // |Z| magnitude at those frequencies, positive
    std::vector<double> phaseRad;       // OPTIONAL Z phase (rad); empty => resistive |Z|
};

// Complex Z of `part` at `freqs` — |Z| by log-log interpolation and (when the
// part carries it) phase by LINEAR interpolation in log f, both off the same
// smooth published curve, so interpolation is legitimate. With no phase the
// result is purely real. Raises outside the curve's coverage: no extrapolation.
inline std::vector<Complex> ferrite_impedance_at(const FerritePart& part,
                                                 const std::vector<double>& freqs) {
    const auto& fp = part.frequenciesHz;
    const auto& zp = part.zOhm;
    const auto& pp = part.phaseRad;
    if (fp.size() < 2 || fp.size() != zp.size()) {
        throw std::invalid_argument(part.name + ": datasheet needs >= 2 aligned (f, |Z|) points");
    }
    if (!pp.empty() && pp.size() != fp.size()) {
        throw std::invalid_argument(part.name + ": phase, when given, must align with the |Z| points");
    }
    for (size_t i = 0; i < fp.size(); ++i) {
        if (!(zp[i] > 0.0)) throw std::invalid_argument(part.name + ": datasheet |Z| must be positive");
        if (i > 0 && !(fp[i] > fp[i - 1])) {
            throw std::invalid_argument(part.name + ": datasheet frequencies must be strictly increasing");
        }
    }
    std::vector<Complex> out;
    out.reserve(freqs.size());
    for (double f : freqs) {
        if (!(f >= fp.front()) || !(f <= fp.back())) {
            throw OutsideCoverage(part.name + ": target frequency outside datasheet |Z| coverage — no extrapolation");
        }
        size_t j = 1;
        while (j + 1 < fp.size() && fp[j] < f) ++j;  // fp[j-1] <= f <= fp[j]
        double lf0 = std::log(fp[j - 1]), lf1 = std::log(fp[j]);
        double lz0 = std::log(zp[j - 1]), lz1 = std::log(zp[j]);
        double t = (lf1 == lf0) ? 0.0 : (std::log(f) - lf0) / (lf1 - lf0);
        double mag = std::exp(lz0 + t * (lz1 - lz0));
        double phase = pp.empty() ? 0.0 : pp[j - 1] + t * (pp[j] - pp[j - 1]);
        out.push_back(Complex(mag * std::cos(phase), mag * std::sin(phase)));
    }
    return out;
}

// CM insertion loss (dB per frequency) of `part` threaded `turns` times, as a
// series CM element between matched cmReferenceOhm terminations. Z scales turns^2.
inline std::vector<double> ferrite_insertion_loss_db(const FerritePart& part, int turns,
                                                     double cmReferenceOhm,
                                                     const std::vector<double>& frequenciesHz) {
    if (turns < 1) throw std::invalid_argument("turns must be >= 1");
    if (!(cmReferenceOhm > 0.0)) throw std::invalid_argument("CM reference impedance must be positive");
    auto z = ferrite_impedance_at(part, frequenciesHz);
    const double scale = static_cast<double>(turns) * static_cast<double>(turns);
    const Complex ref(cmReferenceOhm, 0.0);
    std::vector<double> il;
    il.reserve(z.size());
    for (const Complex& zi : z) {
        // series complex impedance (real-only when the part carries no phase).
        il.push_back(insertion_loss_db(series_impedance(scale * zi), ref, ref));
    }
    return il;
}

struct MitigationChoice {
    std::string partName;
    int turns;
    double worstMarginDb;             // min over the flagged band of (IL - required); >= 0 => met
    bool meetsTarget;
    std::vector<double> insertionLossDb;
};

// Pick the least cable ferrite meeting a CM-attenuation target: fewer turns
// first (simplest install), then earlier parts (order the list small-first).
// Returns the winner, or the best near-miss (meetsTarget false) with its worst
// shortfall — never a silent fail. Throws if nothing is required.
inline MitigationChoice select_cable_mitigation(const std::vector<double>& frequenciesHz,
                                                const std::vector<double>& requiredAttenuationDb,
                                                const std::vector<FerritePart>& parts,
                                                double cmReferenceOhm, int maxTurns = 1) {
    if (frequenciesHz.size() != requiredAttenuationDb.size() || frequenciesHz.empty()) {
        throw std::invalid_argument("frequencies and requirements must be equal-length non-empty arrays");
    }
    if (parts.empty()) throw std::invalid_argument("no candidate parts supplied");
    if (maxTurns < 1) throw std::invalid_argument("maxTurns must be >= 1");
    bool anyNeed = false;
    for (double r : requiredAttenuationDb) if (r > 0.0) anyNeed = true;
    if (!anyNeed) {
        throw std::invalid_argument("no mitigation required — the estimate is under the limit across the band");
    }

    MitigationChoice best;
    bool haveBest = false;
    for (int turns = 1; turns <= maxTurns; ++turns) {
        for (const auto& part : parts) {
            std::vector<double> il;
            try {
                il = ferrite_insertion_loss_db(part, turns, cmReferenceOhm, frequenciesHz);
            } catch (const OutsideCoverage&) {
                // A part whose |Z| curve does not span the flagged band cannot be
                // evaluated over it, so it is simply not a candidate here — skip it.
                // One un-coverable part must never abort the whole selection (with a
                // 776-part catalogue that is a live risk). A MALFORMED curve still
                // throws std::invalid_argument from ferrite_impedance_at and surfaces.
                continue;
            }
            double worst = std::numeric_limits<double>::infinity();
            for (size_t i = 0; i < il.size(); ++i) {
                if (requiredAttenuationDb[i] > 0.0) worst = std::min(worst, il[i] - requiredAttenuationDb[i]);
            }
            MitigationChoice choice{part.name, turns, worst, worst >= 0.0, il};
            if (choice.meetsTarget) return choice;
            if (!haveBest || worst > best.worstMarginDb) {
                best = choice;
                haveBest = true;
            }
        }
    }
    if (!haveBest) {
        // Every supplied part was un-coverable over the flagged band — a real,
        // reportable condition (the caller must widen the catalogue or band),
        // not a silent empty pick.
        throw OutsideCoverage(
            "no candidate ferrite covers the flagged band — every supplied part's |Z| curve stops short of it");
    }
    return best;
}

}  // namespace Hertz
