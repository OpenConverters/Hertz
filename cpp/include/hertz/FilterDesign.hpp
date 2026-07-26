#pragma once
#include <cmath>
#include <numbers>
#include <optional>
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
// A of exactly 0 is legal (resonance AT the design frequency suffices — the
// mode needs no headroom); a NEGATIVE requirement means no filter at all.
inline double cutoff_frequency(double fDesignHz, double aReqDb, int stages) {
    if (stages != 1 && stages != 2) {
        throw std::invalid_argument("ANP015 covers 1- or 2-stage filters");
    }
    if (aReqDb < 0.0) {
        throw std::invalid_argument("required attenuation is negative — the limit is already met");
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
    double fDesignCmHz;
    double fDesignDmHz;
    double fCutoffCmHz;
    double fCutoffDmHz;
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
//
// CM and DM are PHYSICALLY INDEPENDENT networks (choke vs. Y caps; leakage vs.
// X cap), so each gets its own cutoff from ITS OWN requirement at ITS OWN
// design frequency — collapsing to max(A_cm, A_dm) sized the quiet mode from
// the loud mode's problem (224x over-designed chokes in review). The optional
// per-mode design frequencies serve measurement-driven callers whose CM and DM
// critical points sit at different frequencies; both default to
// design_frequency(fSwHz).
inline LineFilterDesign design_line_filter(double fSwHz, double aReqCmDb, double aReqDmDb,
                                           double cYPerLineF, double lDmH, int stages,
                                           const std::vector<double>& lCmCandidates,
                                           const std::vector<double>& cXCandidates,
                                           std::optional<double> fDesignCmHz = std::nullopt,
                                           std::optional<double> fDesignDmHz = std::nullopt) {
    double fDesignDefault = design_frequency(fSwHz);
    double fDesignCm = fDesignCmHz.value_or(fDesignDefault);
    double fDesignDm = fDesignDmHz.value_or(fDesignDefault);
    if (fDesignCm <= 0.0 || fDesignDm <= 0.0) {
        throw std::invalid_argument("per-mode design frequencies must be positive");
    }
    double fCutoffCm = cutoff_frequency(fDesignCm, aReqCmDb, stages);
    double fCutoffDm = cutoff_frequency(fDesignDm, aReqDmDb, stages);

    double cYg = 2.0 * cYPerLineF;
    double lCmRequired = cm_inductance(fCutoffCm, cYg);
    double lCmSelected;
    try {
        lCmSelected = round_up_to(lCmRequired, lCmCandidates);
    } catch (const std::invalid_argument&) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "no CM-choke candidate >= the required %.3g mH — add a larger part, raise "
                      "C_Y, relax the requirement, or loosen the catalog filters",
                      lCmRequired * 1e3);
        throw std::invalid_argument(msg);
    }
    double attenuationCm = achieved_attenuation_db(fDesignCm, lCmSelected, cYg, stages);

    double cXRequired = dm_capacitance(fCutoffDm, lDmH);
    double cXSelected;
    try {
        cXSelected = round_up_to(cXRequired, cXCandidates);
    } catch (const std::invalid_argument&) {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      "no X-capacitor candidate >= the required %.3g uF — add a larger part or "
                      "relax the DM requirement", cXRequired * 1e6);
        throw std::invalid_argument(msg);
    }
    double attenuationDm = achieved_attenuation_db(fDesignDm, lDmH, cXSelected, stages);

    return LineFilterDesign{stages,     fDesignCm,   fDesignDm,   fCutoffCm,     fCutoffDm,
                            cYPerLineF, cYg,         lCmRequired, lCmSelected,   attenuationCm,
                            lDmH,       cXRequired,  cXSelected,  attenuationDm};
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

// Middlebrook input-filter interaction: the filter's output impedance peak
// (characteristic impedance at resonance, undamped) must sit well below the
// converter's negative-incremental input impedance |Z_in| = V_in(min)²/P_in.
// Damping per the Trilogy: R_d = √(L/C) (Q = 1), C_d = 5–10 × C.
struct FilterInteraction {
    double resonanceHz;
    double characteristicImpedanceOhm;
    double converterInputImpedanceOhm;
    double marginDb;                       // 20·log10(Zin/R0): >= 12 dB comfortable, < 6 dB critical
    double dampingResistorOhm;
    double dampingCapacitorMinF;
    double dampingCapacitorMaxF;
};

inline FilterInteraction input_filter_interaction(double inductanceH, double capacitanceF,
                                                  double vInMinV, double pInW) {
    if (inductanceH <= 0.0 || capacitanceF <= 0.0) {
        throw std::invalid_argument("inductance and capacitance must be positive");
    }
    if (vInMinV <= 0.0 || pInW <= 0.0) {
        throw std::invalid_argument("minimum input voltage and input power must be positive");
    }
    double characteristic = std::sqrt(inductanceH / capacitanceF);
    double converterInput = vInMinV * vInMinV / pInW;
    return FilterInteraction{
        resonant_cutoff(inductanceH, capacitanceF),
        characteristic,
        converterInput,
        20.0 * std::log10(converterInput / characteristic),
        characteristic,
        5.0 * capacitanceF,
        10.0 * capacitanceF,
    };
}

}  // namespace Hertz
