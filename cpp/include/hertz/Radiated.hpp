#pragma once
#include <cmath>
#include <stdexcept>
#include <vector>

namespace Hertz {

// Radiated pre-scan SCREENING estimator — the classic electrically-short
// common-mode radiator (Ott / Paul): a cable of length L carrying common-mode
// current I over a ground plane radiates, worst case at resonance-free
// orientation,
//
//     |E| = 1.257e-6 * f * L_eff * I / d     [V/m; f Hz, L m, I A, d m]
//
// with the effective length clamped at lambda/4 above the cable's first
// resonance (a longer cable does not keep radiating proportionally). This is
// the model behind the folklore that ~5 uA of CM current on a 1 m cable is
// enough to fail CISPR 32 Class B at 3 m — and it is a SCREENING estimate:
// real installations resonate and couple +-10-20 dB around it. Callers must
// present it as such; the number is a triage tool, never a measurement.
inline constexpr double RADIATED_MODEL_UNCERTAINTY_DB = 20.0;

inline std::vector<double> radiated_efield_dbuvm(const std::vector<double>& frequenciesHz,
                                                 const std::vector<double>& cmCurrentDbuA,
                                                 double cableLengthM, double distanceM) {
    if (frequenciesHz.size() != cmCurrentDbuA.size()) {
        throw std::invalid_argument("frequency and current arrays differ in length");
    }
    if (frequenciesHz.empty()) {
        throw std::invalid_argument("radiated estimate needs at least one point");
    }
    if (cableLengthM <= 0.0 || distanceM <= 0.0) {
        throw std::invalid_argument("cable length and distance must be positive");
    }
    std::vector<double> efield;
    efield.reserve(frequenciesHz.size());
    for (size_t i = 0; i < frequenciesHz.size(); ++i) {
        double f = frequenciesHz[i];
        if (!(f > 0.0) || !std::isfinite(f) || !std::isfinite(cmCurrentDbuA[i])) {
            throw std::invalid_argument("radiated estimate needs positive finite frequencies and finite currents");
        }
        double lambdaQuarter = 299792458.0 / (4.0 * f);
        double lEff = std::min(cableLengthM, lambdaQuarter);
        double iAmp = std::pow(10.0, cmCurrentDbuA[i] / 20.0) * 1e-6;  // dBuA -> A
        double eVoltPerM = 1.257e-6 * f * lEff * iAmp / distanceM;
        efield.push_back(20.0 * std::log10(eVoltPerM * 1e6));          // V/m -> dBuV/m
    }
    return efield;
}

// Buffer added on top of the raw over-limit when turning the screen into a
// mitigation target — small (the model already carries its own
// RADIATED_MODEL_UNCERTAINTY_DB), and a caller can override.
inline constexpr double RADIATED_TARGET_MARGIN_DB = 6.0;

// Required common-mode CURRENT attenuation (dB per frequency) to bring the
// radiated E-field SCREENING estimate under limitDbuvm. The model is LINEAR in
// I_cm (|E| ~ I_cm), so every dB the field sits over the limit is one dB of
// CM-current suppression to add — this is exactly that requirement, the same
// "attenuation target vs frequency" shape the conducted path feeds a synthesiser
// (required_attenuation_db). The matching consumer for the radiated band is
// Hertz::select_cable_mitigation (CableMitigation.hpp) — a mains filter's choke
// has self-resonated up here, so a cable ferrite is what actually helps.
// Values <= 0 mean already under the limit there. TRIAGE, not a compliant spec.
inline std::vector<double> radiated_cm_attenuation_target_db(
        const std::vector<double>& frequenciesHz, const std::vector<double>& cmCurrentDbuA,
        double cableLengthM, double distanceM, const std::vector<double>& limitDbuvm,
        double marginDb = RADIATED_TARGET_MARGIN_DB) {
    auto efield = radiated_efield_dbuvm(frequenciesHz, cmCurrentDbuA, cableLengthM, distanceM);
    if (limitDbuvm.size() != efield.size()) {
        throw std::invalid_argument("limit array must match the frequency array length");
    }
    std::vector<double> target(efield.size());
    for (size_t i = 0; i < efield.size(); ++i) {
        target[i] = efield[i] - limitDbuvm[i] + marginDb;  // measured - limit + margin
    }
    return target;
}

}  // namespace Hertz
