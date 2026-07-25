#pragma once
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <vector>

#include "hertz/Utils.hpp"

namespace Hertz {

// Single-phase line-filter design per Würth Elektronik application note ANP015
// ("Line filter design for single-phase applications"). Equation numbers refer
// to ANP015 rev. 2024-06. Component values are rounded UP onto an explicit
// candidate list (catalog availability or an E-series); there is no built-in
// default catalog on purpose.

inline constexpr double MIN_MEASURED_FREQUENCY_HZ = 150e3;  // conducted bands start here

// Frequency the filter is designed at: f_sw, or its first harmonic >= 150 kHz.
inline double design_frequency(double fSwHz) {
    if (fSwHz <= 0.0) {
        throw std::invalid_argument("switching frequency must be positive");
    }
    if (fSwHz >= MIN_MEASURED_FREQUENCY_HZ) {
        return fSwHz;
    }
    return std::ceil(MIN_MEASURED_FREQUENCY_HZ / fSwHz) * fSwHz;
}

// A_req = measured - limit + margin (ANP015 §1; 10 dB is the note's buffer).
// Negative results mean the limit is already met at that frequency.
inline double required_attenuation_db(double measuredDbuv, double limitDbuv, double marginDb = 10.0) {
    return measuredDbuv - limitDbuv + marginDb;
}

// f_CO = f_design / 10^(A/(40·n)) — eqs. (4) single stage, (11) two stage.
inline double cutoff_frequency(double fDesignHz, double aReqDb, int stages) {
    if (stages != 1 && stages != 2) {
        throw std::invalid_argument("ANP015 covers 1- or 2-stage filters");
    }
    if (aReqDb <= 0.0) {
        throw std::invalid_argument("required attenuation must be positive — the limit is already met");
    }
    return fDesignHz / std::pow(10.0, aReqDb / (40.0 * stages));
}

inline double resonant_cutoff(double inductanceH, double capacitanceF) {
    if (inductanceH <= 0.0 || capacitanceF <= 0.0) {
        throw std::invalid_argument("inductance and capacitance must be positive");
    }
    return 1.0 / (2.0 * std::numbers::pi * std::sqrt(inductanceH * capacitanceF));
}

// L_CM = 1/((2π f_CO)² C_YG) — eq. (7); C_YG = 2·C_Y per line pair, eq. (6).
inline double cm_inductance(double fCutoffHz, double cYgF) {
    if (fCutoffHz <= 0.0 || cYgF <= 0.0) {
        throw std::invalid_argument("cutoff frequency and C_YG must be positive");
    }
    double w = 2.0 * std::numbers::pi * fCutoffHz;
    return 1.0 / (w * w * cYgF);
}

// C_X = 1/((2π f_CO)² L_DM) — eq. (9).
inline double dm_capacitance(double fCutoffHz, double lDmH) {
    if (fCutoffHz <= 0.0 || lDmH <= 0.0) {
        throw std::invalid_argument("cutoff frequency and L_DM must be positive");
    }
    double w = 2.0 * std::numbers::pi * fCutoffHz;
    return 1.0 / (w * w * lDmH);
}

// L from the inductive region of a measured DM impedance curve — eq. (8).
inline double leakage_inductance_from_impedance(double zMagOhm, double fHz) {
    if (zMagOhm <= 0.0 || fHz <= 0.0) {
        throw std::invalid_argument("impedance and frequency must be positive");
    }
    return zMagOhm / (2.0 * std::numbers::pi * fHz);
}

// A = 40·n·log10(f_design/f_CO) with the actually selected L and C. Asymptotic
// slope estimate; negative when the resonance sits above f_design.
inline double achieved_attenuation_db(double fDesignHz, double inductanceH, double capacitanceF,
                                      int stages) {
    if (stages != 1 && stages != 2) {
        throw std::invalid_argument("ANP015 covers 1- or 2-stage filters");
    }
    return 40.0 * stages * std::log10(fDesignHz / resonant_cutoff(inductanceH, capacitanceF));
}

struct LineFilterDesign {
    int stages;
    double fDesignHz;
    double fCutoffTargetHz;
    double cYPerLineF;
    double cYgF;
    double lCmRequiredH;
    double lCmSelectedH;
    double attenuationCmDb;
    double lDmH;
    double cXRequiredF;
    double cXSelectedF;
    double attenuationDmDb;
};

// Full ANP015 sizing pass. cYPerLineF: the chosen Y capacitor per line (bounded
// by leakage current, see y_capacitor_leakage_current). lDmH: DM (leakage)
// inductance of the CM choke. Both stages of a 2-stage filter use identical
// components (ANP015 §3).
inline LineFilterDesign design_line_filter(double fSwHz, double aReqCmDb, double aReqDmDb,
                                           double cYPerLineF, double lDmH, int stages,
                                           const std::vector<double>& lCmCandidates,
                                           const std::vector<double>& cXCandidates) {
    double fDesign = design_frequency(fSwHz);
    double aWorst = std::max(aReqCmDb, aReqDmDb);
    double fCutoff = cutoff_frequency(fDesign, aWorst, stages);

    double cYg = 2.0 * cYPerLineF;
    double lCmRequired = cm_inductance(fCutoff, cYg);
    double lCmSelected = round_up_to(lCmRequired, lCmCandidates);
    double attenuationCm = achieved_attenuation_db(fDesign, lCmSelected, cYg, stages);

    double cXRequired = dm_capacitance(fCutoff, lDmH);
    double cXSelected = round_up_to(cXRequired, cXCandidates);
    double attenuationDm = achieved_attenuation_db(fDesign, lDmH, cXSelected, stages);

    return LineFilterDesign{stages,       fDesign,     fCutoff,       cYPerLineF,
                            cYg,          lCmRequired, lCmSelected,   attenuationCm,
                            lDmH,         cXRequired,  cXSelected,    attenuationDm};
}

// Protective-earth leakage current — ANP015 eqs. (29)-(30). Apply worst-case
// tolerances (e.g. V+10 %, C+20 %) in the arguments; this computes the plain
// formula only.
inline double y_capacitor_leakage_current(double vGridRms, double fGridHz, double cYLineF,
                                          double cYNeutralF, double cXTotalF) {
    if (vGridRms <= 0.0 || fGridHz <= 0.0) {
        throw std::invalid_argument("grid voltage and frequency must be positive");
    }
    if (cYLineF < 0.0 || cYNeutralF < 0.0 || cXTotalF < 0.0) {
        throw std::invalid_argument("capacitances must be non-negative");
    }
    double series =
        cXTotalF > 0.0 ? cXTotalF * cYNeutralF / (cXTotalF + cYNeutralF) : 0.0;
    return vGridRms * 2.0 * std::numbers::pi * fGridHz * (cYLineF + series);
}

// Largest X-cap discharge resistor meeting v(t_max) <= v_safe — eq. (34).
inline double max_discharge_resistance(double cTotalF, double vStart, double vSafe, double tMaxS) {
    if (!(0.0 < vSafe && vSafe < vStart)) {
        throw std::invalid_argument("need 0 < v_safe < v_start");
    }
    if (cTotalF <= 0.0 || tMaxS <= 0.0) {
        throw std::invalid_argument("capacitance and time must be positive");
    }
    return tMaxS / (cTotalF * std::log(vStart / vSafe));
}

// Continuous dissipation of the discharge resistor — eq. (35).
inline double discharge_resistor_power(double vGridRms, double resistanceOhm) {
    if (resistanceOhm <= 0.0) {
        throw std::invalid_argument("resistance must be positive");
    }
    return vGridRms * vGridRms / resistanceOhm;
}

}  // namespace Hertz
