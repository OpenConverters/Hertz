// Emscripten/embind surface of the Hertz engine for the web GUI.
// JSON strings for structured data, typed arrays (via emscripten::val) for bulk
// samples. Every computation lives in the C++ headers — this file only marshals.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "hertz/CableMitigation.hpp"
#include "hertz/CombDetection.hpp"
#include "hertz/Detector.hpp"
#include "hertz/FilterBlock.hpp"
#include "hertz/FilterDesign.hpp"
#include "hertz/Limits.hpp"
#include "hertz/Lisn.hpp"
#include "hertz/Network.hpp"
#include "hertz/Radiated.hpp"
#include "hertz/Separation.hpp"
#include "hertz/SpiceDeck.hpp"
#include "hertz/Traces.hpp"
#include "hertz/Utils.hpp"

using json = nlohmann::json;

namespace {

Hertz::Detector detector_from_string(const std::string& name) {
    if (name == "peak") return Hertz::Detector::PEAK;
    if (name == "quasi_peak") return Hertz::Detector::QUASI_PEAK;
    if (name == "average") return Hertz::Detector::AVERAGE;
    throw std::invalid_argument("unknown detector " + name);
}

Hertz::LimitLine limit_line_for(const std::string& standardId, const std::string& detectorName) {
    Hertz::Detector detector = detector_from_string(detectorName);
    if (standardId == "cispr32_class_b" || standardId == "cispr32_class_a") {
        if (detector == Hertz::Detector::PEAK) {
            throw std::invalid_argument("CISPR 32 defines no conducted peak limit");
        }
        if (standardId == "cispr32_class_b") {
            return detector == Hertz::Detector::AVERAGE ? Hertz::cispr32_class_b_mains_avg()
                                                        : Hertz::cispr32_class_b_mains_qp();
        }
        return detector == Hertz::Detector::AVERAGE ? Hertz::cispr32_class_a_mains_avg()
                                                    : Hertz::cispr32_class_a_mains_qp();
    }
    const std::string radPrefix = "cispr32_rad_";
    if (standardId.rfind(radPrefix, 0) == 0) {
        // ids: cispr32_rad_b_10m / cispr32_rad_b_3m / cispr32_rad_a_10m / cispr32_rad_a_3m
        if (detector != Hertz::Detector::QUASI_PEAK) {
            throw std::invalid_argument("CISPR 32 radiated limits are quasi-peak");
        }
        char cls = standardId[radPrefix.size()];
        double dist = standardId.find("_3m") != std::string::npos ? 3.0 : 10.0;
        return Hertz::cispr32_radiated(cls, dist);
    }
    const std::string prefix = "cispr25_class_";
    if (standardId.rfind(prefix, 0) == 0) {
        int emissionClass = std::stoi(standardId.substr(prefix.size()));
        return Hertz::cispr25_conducted_voltage(emissionClass, detector);
    }
    throw std::invalid_argument("unknown standard " + standardId);
}

std::vector<double> to_vector(const emscripten::val& value) {
    return emscripten::convertJSArrayToNumberVector<double>(value);
}


// Marshal C++ exceptions as {"error": message} JSON at the boundary — the GUI
// checks for it and rethrows as a JS Error. Internally the engine still throws.
template <typename F>
std::string guarded(F&& body) {
    try {
        return body();
    } catch (const std::exception& e) {
        return json{{"error", e.what()}}.dump();
    }
}

const Hertz::CisprBand& band_from_string(const std::string& name) {
    if (name == "A") return Hertz::BAND_A;
    if (name == "B") return Hertz::BAND_B;
    if (name == "C") return Hertz::BAND_C;
    if (name == "D") return Hertz::BAND_D;
    throw std::invalid_argument("unknown CISPR band " + name);
}

std::string engine_version() {
    return "0.1.0";
}

// levelColumn: 1-based numeric column to judge (0 = unset — REQUIRED for
// multi-trace exports carrying more than two numeric columns).
std::string parse_spectrum_csv_js(const std::string& content, const std::string& freqUnit,
                                  const std::string& levelUnit, int levelColumn)  {
    return guarded([&]() -> std::string {
    auto trace = Hertz::parse_spectrum_csv(
        content, freqUnit.empty() ? std::nullopt : std::make_optional(freqUnit),
        levelUnit.empty() ? std::nullopt : std::make_optional(levelUnit), 50.0,
        levelColumn == 0 ? std::nullopt : std::make_optional(levelColumn));
    return json{{"frequenciesHz", trace.frequenciesHz}, {"levelsDbuv", trace.levelsDbuv},
                {"levelUnit", trace.levelUnit}}.dump();
});
}

// Numeric-column inventory so a GUI can offer the level-column choice.
std::string spectrum_csv_columns_js(const std::string& content) {
    return guarded([&]() -> std::string {
        auto columns = Hertz::spectrum_csv_columns(content);
        return json{{"count", columns.count},
                    {"frequencyColumn", columns.frequencyColumn},
                    {"names", columns.names}}.dump();
    });
}

// Trace vs. limit: per-point limit/margin arrays, worst offender, pass verdict,
// and the ANP015 required attenuation (limit margin + 10 dB buffer).
std::string limit_analysis_js(const std::string& standardId, const std::string& detectorName,
                              const std::string& freqsJson, const std::string& levelsJson,
                              double marginBufferDb)  {
    return guarded([&]() -> std::string {
    auto line = limit_line_for(standardId, detectorName);
    std::vector<double> freqs = json::parse(freqsJson).get<std::vector<double>>();
    std::vector<double> levels = json::parse(levelsJson).get<std::vector<double>>();
    if (freqs.size() != levels.size()) {
        throw std::invalid_argument("frequency and level arrays differ in length");
    }
    json covered = json::array();
    json limits = json::array();
    json margins = json::array();
    bool pass = true;
    bool anyCovered = false;
    double worstMargin = std::numeric_limits<double>::infinity();
    size_t worstIndex = 0;
    for (size_t i = 0; i < freqs.size(); ++i) {
        if (!std::isfinite(freqs[i]) || !std::isfinite(levels[i])) {
            throw std::invalid_argument("non-finite trace value at point " + std::to_string(i + 1) +
                                        " — clean the export before judging it");
        }
        if (line.covers(freqs[i])) {
            double limit = line.level(freqs[i]);
            double margin = limit - levels[i];
            covered.push_back(true);
            limits.push_back(limit);
            margins.push_back(margin);
            anyCovered = true;
            if (margin < 0.0) pass = false;
            if (margin < worstMargin) {
                worstMargin = margin;
                worstIndex = i;
            }
        } else {
            covered.push_back(false);
            limits.push_back(nullptr);
            margins.push_back(nullptr);
        }
    }
    if (!anyCovered) {
        throw std::invalid_argument("no trace point falls inside the selected limit's bands");
    }
    json unswept = json::array();
    for (const auto& [f0, f1] : Hertz::unswept_regions(line, freqs)) {
        unswept.push_back({f0, f1});
    }
    double requiredAttenuation =
        Hertz::required_attenuation_db(levels[worstIndex], line.level(freqs[worstIndex]),
                                       marginBufferDb);
    return json{{"name", line.name()},
                {"covered", covered},
                {"limitsDbuv", limits},
                {"marginsDb", margins},
                {"pass", pass},
                {"worst",
                 {{"index", worstIndex},
                  {"frequencyHz", freqs[worstIndex]},
                  {"levelDbuv", levels[worstIndex]},
                  {"limitDbuv", line.level(freqs[worstIndex])},
                  {"marginDb", worstMargin}}},
                {"requiredAttenuationDb", requiredAttenuation},
                {"unsweptHz", unswept}}
        .dump();
});
}

// Polyline of a limit line over [fMin, fMax] for drawing: covered runs of
// {f, level} points sampled per decade (log-linear segments stay exact at
// their endpoints; sampling only smooths the display).
std::string limit_polyline_js(const std::string& standardId, const std::string& detectorName,
                              double fMinHz, double fMaxHz, int pointsPerDecade)  {
    return guarded([&]() -> std::string {
    auto line = limit_line_for(standardId, detectorName);
    json runs = json::array();
    for (const auto& segmentRun : Hertz::limit_polyline_runs(line, fMinHz, fMaxHz, pointsPerDecade)) {
        json run = json::array();
        for (const auto& [f, v] : segmentRun) {
            run.push_back({{"f", f}, {"v", v}});
        }
        runs.push_back(run);
    }
    return json{{"name", line.name()}, {"runs", runs}}.dump();
});
}

std::string design_filter_js(const std::string& paramsJson)  {
    return guarded([&]() -> std::string {
    json params = json::parse(paramsJson);
    double lDm;
    if (params.contains("lDmH")) {
        lDm = params.at("lDmH").get<double>();
    } else {
        lDm = Hertz::leakage_inductance_from_impedance(params.at("dmImpedanceOhm").get<double>(),
                                                       params.at("dmImpedanceFrequencyHz").get<double>());
    }
    auto optionalHz = [&params](const char* key) -> std::optional<double> {
        return params.contains(key) && !params.at(key).is_null()
                   ? std::make_optional(params.at(key).get<double>())
                   : std::nullopt;
    };
    int nLines = params.value("nLines", 2);
    auto design = Hertz::design_line_filter(
        params.at("fSwHz").get<double>(), params.at("aReqCmDb").get<double>(),
        params.at("aReqDmDb").get<double>(), params.at("cYPerLineF").get<double>(), lDm,
        params.at("stages").get<int>(), params.at("lCmCandidatesH").get<std::vector<double>>(),
        params.at("cXCandidatesF").get<std::vector<double>>(),
        optionalHz("fDesignCmHz"), optionalHz("fDesignDmHz"), nLines);

    json result{{"stages", design.stages},
                {"fDesignCmHz", design.fDesignCmHz},
                {"fDesignDmHz", design.fDesignDmHz},
                {"fCutoffCmHz", design.fCutoffCmHz},
                {"fCutoffDmHz", design.fCutoffDmHz},
                {"cYPerLineF", design.cYPerLineF},
                {"cYgF", design.cYgF},
                {"lCmRequiredH", design.lCmRequiredH},
                {"lCmSelectedH", design.lCmSelectedH},
                {"attenuationCmDb", design.attenuationCmDb},
                {"lDmH", design.lDmH},
                {"cXRequiredF", design.cXRequiredF},
                {"cXSelectedF", design.cXSelectedF},
                {"attenuationDmDb", design.attenuationDmDb},
                {"nLines", design.nLines},
                {"cXDmFactor", design.cXDmFactor},
                {"lCmFloorFromLeakage", design.lCmFloorFromLeakage}};

    if (params.contains("grid")) {
        const json& grid = params.at("grid");
        double vRms = grid.at("vRms").get<double>();
        double fGrid = grid.at("fHz").get<double>();
        // 3-phase grids state vRms LINE-TO-LINE; a Y capacitor (and a touching
        // person) sees phase-to-earth, and a star X capacitor phase-to-neutral.
        // Delta X capacitors sit across the full line-to-line voltage.
        double vPhase = nLines >= 3 ? vRms / std::sqrt(3.0) : vRms;
        double vAcrossX = nLines == 3 ? vRms : vPhase;
        // Worst case V+10 %, C+20 % applied consistently to BOTH safety rows.
        // Touch current per the IEC 60990 model: neutral sits at earth, so the
        // compliance figure is the single line-side Y capacitor (the naive
        // 2x-sum halves the Y budget a designer believes is available).
        double cXTotal = design.cXSelectedF * design.stages * 1.2;
        double cY = design.cYPerLineF * 1.2 * design.stages;
        result["leakageCurrentA"] =
            Hertz::y_capacitor_leakage_current(vPhase * 1.1, fGrid, cY, 0.0, cXTotal);
        double cTotalWorst = design.cXSelectedF * design.stages * 1.2;
        double rMax = Hertz::max_discharge_resistance(
            cTotalWorst, vAcrossX * 1.1 * std::numbers::sqrt2, grid.at("vSafe").get<double>(),
            grid.at("tDischargeS").get<double>());
        double rChosen = Hertz::round_down_to_series(rMax, Hertz::E24);
        result["dischargeResistorMaxOhm"] = rMax;
        result["dischargeResistorOhm"] = rChosen;
        result["dischargeResistorPowerW"] = Hertz::discharge_resistor_power(vAcrossX * 1.1, rChosen);
    }
    return result.dump();
});
}

// SPICE netlist of the designed filter between a noise port and the LISN, ready
// for Kirchhoff/ngspice/LTspice (.ac analysis of insertion loss).
std::string filter_spice_netlist_js(const std::string& designJson, const std::string& lisnKind,
                                    const std::string& mode)  {
    return guarded([&]() -> std::string {
    json design = json::parse(designJson);
    Hertz::Lisn lisn = lisnKind == "cispr25" ? Hertz::cispr25_lisn() : Hertz::cispr16_lisn();
    return Hertz::filter_spice_deck(
        design.at("stages").get<int>(), design.at("lCmSelectedH").get<double>(),
        design.at("cXSelectedF").get<double>(), design.at("cYPerLineF").get<double>(),
        design.at("lDmH").get<double>(), lisn, mode, design.value("nLines", 2));
});
}

// Reference bench for the ngspice round-trip (#299): same drive + LISNs, no filter.
std::string filter_reference_netlist_js(const std::string& lisnKind, const std::string& mode,
                                        int nLines)  {
    return guarded([&]() -> std::string {
    Hertz::Lisn lisn = lisnKind == "cispr25" ? Hertz::cispr25_lisn() : Hertz::cispr16_lisn();
    return Hertz::lisn_reference_deck(lisn, mode, nLines);
});
}

// The analytic ABCD twin evaluated under the DECK's actual terminations, at the
// caller's (ngspice-sweep) frequencies — see SpiceDeck.hpp deck_abcd_il.
std::string deck_abcd_il_js(const std::string& designJson, const std::string& lisnKind,
                            const std::string& mode, const emscripten::val& freqsArray)  {
    return guarded([&]() -> std::string {
    json design = json::parse(designJson);
    Hertz::Lisn lisn = lisnKind == "cispr25" ? Hertz::cispr25_lisn() : Hertz::cispr16_lisn();
    auto il = Hertz::deck_abcd_il(
        lisn, mode, design.value("nLines", 2), design.at("stages").get<int>(),
        design.at("lCmSelectedH").get<double>(), design.at("cYPerLineF").get<double>(),
        design.at("lDmH").get<double>(), design.at("cXSelectedF").get<double>(),
        design.value("cXDmFactor", 1.0), to_vector(freqsArray));
    return json(il).dump();
});
}

std::string lisn_data_js(const std::string& kind, double fMinHz, double fMaxHz,
                         int pointsPerDecade)  {
    return guarded([&]() -> std::string {
    if (kind != "cispr16" && kind != "cispr25") {
        throw std::invalid_argument("unknown LISN kind " + kind);
    }
    Hertz::Lisn lisn = kind == "cispr25" ? Hertz::cispr25_lisn() : Hertz::cispr16_lisn();
    if (!(0.0 < fMinHz && fMinHz < fMaxHz) || pointsPerDecade < 2) {
        throw std::invalid_argument("bad frequency range");
    }
    json freqs = json::array();
    json magnitude = json::array();
    json phase = json::array();
    double logMin = std::log10(fMinHz);
    double logMax = std::log10(fMaxHz);
    // Guard steps>=1 (see Network.hpp): a sub-step span would truncate to 0 and make
    // i/steps = 0/0 = NaN in the loop below.
    int steps = std::max(1, static_cast<int>((logMax - logMin) * pointsPerDecade));
    for (int i = 0; i <= steps; ++i) {
        double f = std::pow(10.0, logMin + (logMax - logMin) * i / steps);
        std::complex<double> z = lisn.eut_impedance(f);
        freqs.push_back(f);
        magnitude.push_back(std::abs(z));
        phase.push_back(std::arg(z) * 180.0 / std::numbers::pi);
    }
    return json{{"name", lisn.name},
                {"frequenciesHz", freqs},
                {"impedanceOhm", magnitude},
                {"phaseDeg", phase},
                {"subckt", lisn.to_spice_subckt()}}
        .dump();
});
}

std::string separate_js(const emscripten::val& lineArray, const emscripten::val& neutralArray)  {
    return guarded([&]() -> std::string {
    auto [cm, dm] = Hertz::separate(to_vector(lineArray), to_vector(neutralArray));
    return json{{"commonMode", cm}, {"differentialMode", dm}}.dump();
});
}

std::string measure_waveform_js(const emscripten::val& samplesArray, double fsHz,
                                const std::string& bandName, double overlap)  {
    return guarded([&]() -> std::string {
    auto reading = Hertz::measure(to_vector(samplesArray), fsHz, band_from_string(bandName), overlap);
    return json{{"frequenciesHz", reading.frequenciesHz},
                {"peakDbuv", reading.peakDbuv},
                {"quasiPeakDbuv", reading.quasiPeakDbuv},
                {"averageDbuv", reading.averageDbuv}}
        .dump();
});
}


std::string detect_comb_js(const std::string& freqsJson, const std::string& levelsJson) {
    return guarded([&]() -> std::string {
        auto result = Hertz::detect_comb(json::parse(freqsJson).get<std::vector<double>>(),
                                         json::parse(levelsJson).get<std::vector<double>>());
        json harmonics = json::array();
        for (const auto& h : result.harmonics) {
            harmonics.push_back({{"order", h.order}, {"frequencyHz", h.frequencyHz},
                                 {"levelDbuv", h.levelDbuv}});
        }
        json residuals = json::array();
        for (const auto& [f, level] : result.residualPeaks) {
            residuals.push_back({{"frequencyHz", f}, {"levelDbuv", level}});
        }
        return json{{"found", result.found},
                    {"fSwHz", result.found ? json(result.fSwHz) : json(nullptr)},
                    {"confidence", result.confidence},
                    {"coverage", result.coverage},
                    {"harmonics", harmonics},
                    {"residualPeaks", residuals}}.dump();
    });
}

std::string insertion_loss_curves_js(const std::string& paramsJson) {
    return guarded([&]() -> std::string {
        json params = json::parse(paramsJson);
        auto curves = Hertz::insertion_loss_curves(
            params.at("inductanceH").get<double>(), params.at("capacitanceF").get<double>(),
            params.at("stages").get<int>(), params.at("referenceImpedanceOhm").get<double>(),
            params.at("fMinHz").get<double>(), params.at("fMaxHz").get<double>(),
            params.value("pointsPerDecade", 40),
            params.value("capEslH", 0.0), params.value("capEsrOhm", 0.0));
        return json{{"frequenciesHz", curves.frequenciesHz},
                    {"standardDb", curves.standardDb},
                    {"worstCaseDb", curves.worstCaseDb}}.dump();
    });
}

std::string measured_il_curves_js(const std::string& freqsJson, const std::string& zReJson,
                                  const std::string& zImJson, double cShuntF, int stages,
                                  double referenceImpedanceOhm) {
    return guarded([&]() -> std::string {
        auto curves = Hertz::tabulated_series_il_curves(
            json::parse(freqsJson).get<std::vector<double>>(),
            json::parse(zReJson).get<std::vector<double>>(),
            json::parse(zImJson).get<std::vector<double>>(), cShuntF, stages,
            referenceImpedanceOhm);
        return json{{"frequenciesHz", curves.frequenciesHz},
                    {"standardDb", curves.standardDb},
                    {"worstCaseDb", curves.worstCaseDb}}.dump();
    });
}

std::string input_filter_interaction_js(double inductanceH, double capacitanceF, double vInMinV,
                                        double pInW) {
    return guarded([&]() -> std::string {
        auto r = Hertz::input_filter_interaction(inductanceH, capacitanceF, vInMinV, pInW);
        return json{{"resonanceHz", r.resonanceHz},
                    {"characteristicImpedanceOhm", r.characteristicImpedanceOhm},
                    {"converterInputImpedanceOhm", r.converterInputImpedanceOhm},
                    {"marginDb", r.marginDb},
                    {"dampingResistorOhm", r.dampingResistorOhm},
                    {"dampingCapacitorMinF", r.dampingCapacitorMinF},
                    {"dampingCapacitorMaxF", r.dampingCapacitorMaxF}}.dump();
    });
}

std::string radiated_estimate_js(const std::string& freqsJson, const std::string& dbuaJson,
                                 double cableLengthM, double distanceM) {
    return guarded([&]() -> std::string {
        auto efield = Hertz::radiated_efield_dbuvm(
            json::parse(freqsJson).get<std::vector<double>>(),
            json::parse(dbuaJson).get<std::vector<double>>(), cableLengthM, distanceM);
        return json{{"efieldDbuvm", efield},
                    {"modelUncertaintyDb", Hertz::RADIATED_MODEL_UNCERTAINTY_DB}}.dump();
    });
}

// Radiated screen -> common-mode CURRENT attenuation target (dB vs frequency)
// plus the governing (worst, frequency) design point when any band is over.
std::string radiated_cm_target_js(const std::string& freqsJson, const std::string& dbuaJson,
                                  double cableLengthM, double distanceM,
                                  const std::string& limitJson, double marginDb) {
    return guarded([&]() -> std::string {
        auto freqs = json::parse(freqsJson).get<std::vector<double>>();
        auto target = Hertz::radiated_cm_attenuation_target_db(
            freqs, json::parse(dbuaJson).get<std::vector<double>>(), cableLengthM, distanceM,
            json::parse(limitJson).get<std::vector<double>>(), marginDb);
        bool needed = false;
        for (double t : target) if (t > 0.0) needed = true;
        json out{{"target", target}, {"needed", needed},
                 {"modelUncertaintyDb", Hertz::RADIATED_MODEL_UNCERTAINTY_DB}};
        if (needed) {
            auto governing = Hertz::governing_requirement(freqs, target);
            out["governingDb"] = governing.attenuationDb;
            out["governingHz"] = governing.frequencyHz;
        }
        return out.dump();
    });
}

// Cable ferrite / CM-choke picker against a CM-attenuation target.
std::string cable_mitigation_js(const std::string& freqsJson, const std::string& reqJson,
                                const std::string& partsJson, double cmReferenceOhm, int maxTurns) {
    return guarded([&]() -> std::string {
        std::vector<Hertz::FerritePart> parts;
        for (const auto& p : json::parse(partsJson)) {
            // phaseRad is optional: present for real catalog curves (complex Z),
            // absent for magnitude-only parts (treated as resistive |Z|).
            std::vector<double> phase;
            if (p.contains("phaseRad") && !p.at("phaseRad").is_null()) {
                phase = p.at("phaseRad").get<std::vector<double>>();
            }
            parts.push_back({p.at("name").get<std::string>(),
                             p.at("frequenciesHz").get<std::vector<double>>(),
                             p.at("zOhm").get<std::vector<double>>(),
                             phase});
        }
        auto choice = Hertz::select_cable_mitigation(
            json::parse(freqsJson).get<std::vector<double>>(),
            json::parse(reqJson).get<std::vector<double>>(), parts, cmReferenceOhm, maxTurns);
        return json{{"partName", choice.partName}, {"turns", choice.turns},
                    {"worstMarginDb", choice.worstMarginDb}, {"meetsTarget", choice.meetsTarget},
                    {"insertionLossDb", choice.insertionLossDb}}.dump();
    });
}


// The filter BLOCK (plan S3/S4): joint layout+BOM optimization, layout-aware
// curves and every export, in one call — the browser gets a characterized
// part, not a calculator answer.
std::string filter_block_js(const std::string& paramsJson) {
    return guarded([&]() -> std::string {
    json p = json::parse(paramsJson);
    double lDm;
    if (p.contains("lDmH")) {
        lDm = p.at("lDmH").get<double>();
    } else {
        lDm = Hertz::leakage_inductance_from_impedance(
            p.at("dmImpedanceOhm").get<double>(),
            p.at("dmImpedanceFrequencyHz").get<double>());
    }
    auto fb = Hertz::layout::optimize_filter_block(
        p.at("fSwHz").get<double>(), p.at("aReqCmDb").get<double>(),
        p.at("aReqDmDb").get<double>(), p.at("cYPerLineF").get<double>(), lDm,
        p.at("ratedCurrentA").get<double>(),
        p.at("lCmCandidatesH").get<std::vector<double>>(),
        p.at("cXCandidatesF").get<std::vector<double>>(),
        p.value("maxStages", 2), p.value("capEslH", 5e-9),
        p.value("capEsrOhm", 0.02));
    auto curves = Hertz::layout::block_curves(fb.design, fb.par);
    const auto& d = fb.design;
    json out{
        {"meets", fb.meets},
        {"escalatedStages", fb.escalatedStages},
        {"iterations", fb.iterations},
        {"layoutAttenCmDb", fb.layoutAttenCmDb},
        {"layoutAttenDmDb", fb.layoutAttenDmDb},
        {"partGapMm", fb.params.partGapmm},
        {"design",
         {{"stages", d.stages},
          {"fDesignCmHz", d.fDesignCmHz},
          {"fDesignDmHz", d.fDesignDmHz},
          {"lCmSelectedH", d.lCmSelectedH},
          {"cXSelectedF", d.cXSelectedF},
          {"cYPerLineF", d.cYPerLineF},
          {"lDmH", d.lDmH},
          {"nLines", d.nLines}}},
        {"board",
         {{"kicadPcb", fb.board.kicadPcb},
          {"widthMm", fb.board.boardWmm},
          {"heightMm", fb.board.boardHmm},
          {"parts", fb.board.parts},
          {"minLNGapMm", fb.board.minLNGapmm},
          {"minPEGapMm", fb.board.minPEGapmm}}},
        {"parasitics",
         {{"mDmNh", fb.par.mDmNh},
          {"xConnNh", fb.par.xConnNh},
          {"yConnNh", fb.par.yConnNh},
          {"peSpineNh", fb.par.peSpineNh},
          {"band",
           "partials ~+/-30%; bypass M ~+/-50% (FastHenry-anchored on the "
           "template: stub 1%, pair M 1% with the closing-vertical term)"}}},
        {"curves",
         {{"fHz", curves.fHz},
          {"dmDb", curves.dmDb},
          {"dmIdealDb", curves.dmIdealDb},
          {"cmDb", curves.cmDb},
          {"cmIdealDb", curves.cmIdealDb},
          {"dmFloorDb", curves.dmFloorDb}}},
        {"spice", Hertz::layout::block_spice_subckt(fb.design, fb.par)},
        {"s2pDm", Hertz::layout::touchstone_s2p(fb.design, fb.par, "dm")},
        {"s2pCm", Hertz::layout::touchstone_s2p(fb.design, fb.par, "cm")},
        {"bomCsv", Hertz::layout::block_bom_csv(fb.design)}};
    return out.dump();
});
}

}  // namespace

EMSCRIPTEN_BINDINGS(hertz) {
    emscripten::function("version", &engine_version);
    emscripten::function("parseSpectrumCsv", &parse_spectrum_csv_js);
    emscripten::function("spectrumCsvColumns", &spectrum_csv_columns_js);
    emscripten::function("limitAnalysis", &limit_analysis_js);
    emscripten::function("limitPolyline", &limit_polyline_js);
    emscripten::function("designFilter", &design_filter_js);
    emscripten::function("filterSpiceNetlist", &filter_spice_netlist_js);
    emscripten::function("filterReferenceNetlist", &filter_reference_netlist_js);
    emscripten::function("deckAbcdIl", &deck_abcd_il_js);
    emscripten::function("lisnData", &lisn_data_js);
    emscripten::function("separateTraces", &separate_js);
    emscripten::function("measureWaveform", &measure_waveform_js);
    emscripten::function("detectComb", &detect_comb_js);
    emscripten::function("insertionLossCurves", &insertion_loss_curves_js);
    emscripten::function("inputFilterInteraction", &input_filter_interaction_js);
    emscripten::function("measuredIlCurves", &measured_il_curves_js);
    emscripten::function("radiatedEstimate", &radiated_estimate_js);
    emscripten::function("radiatedCmTarget", &radiated_cm_target_js);
    emscripten::function("cableMitigation", &cable_mitigation_js);
    emscripten::function("filterBlock", &filter_block_js);
}
