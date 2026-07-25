// Emscripten/embind surface of the Hertz engine for the web GUI.
// JSON strings for structured data, typed arrays (via emscripten::val) for bulk
// samples. Every computation lives in the C++ headers — this file only marshals.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <cmath>
#include <complex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "hertz/CombDetection.hpp"
#include "hertz/Detector.hpp"
#include "hertz/FilterDesign.hpp"
#include "hertz/Limits.hpp"
#include "hertz/Lisn.hpp"
#include "hertz/Network.hpp"
#include "hertz/Separation.hpp"
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
    if (standardId == "cispr32_class_b") {
        return detector == Hertz::Detector::AVERAGE ? Hertz::cispr32_class_b_mains_avg()
                                                    : Hertz::cispr32_class_b_mains_qp();
    }
    if (standardId == "cispr32_class_a") {
        return detector == Hertz::Detector::AVERAGE ? Hertz::cispr32_class_a_mains_avg()
                                                    : Hertz::cispr32_class_a_mains_qp();
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

std::string parse_spectrum_csv_js(const std::string& content, const std::string& freqUnit,
                                  const std::string& levelUnit)  {
    return guarded([&]() -> std::string {
    auto trace = Hertz::parse_spectrum_csv(
        content, freqUnit.empty() ? std::nullopt : std::make_optional(freqUnit),
        levelUnit.empty() ? std::nullopt : std::make_optional(levelUnit));
    return json{{"frequenciesHz", trace.frequenciesHz}, {"levelsDbuv", trace.levelsDbuv}}.dump();
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
                {"requiredAttenuationDb", requiredAttenuation}}
        .dump();
});
}

// Polyline of a limit line over [fMin, fMax] for drawing: covered runs of
// {f, level} points sampled per decade (log-linear segments stay exact at
// their endpoints; sampling only smooths the display).
std::string limit_polyline_js(const std::string& standardId, const std::string& detectorName,
                              double fMinHz, double fMaxHz, int pointsPerDecade)  {
    return guarded([&]() -> std::string {
    if (!(0.0 < fMinHz && fMinHz < fMaxHz) || pointsPerDecade < 2) {
        throw std::invalid_argument("bad polyline range");
    }
    auto line = limit_line_for(standardId, detectorName);
    json runs = json::array();
    json run = json::array();
    double logMin = std::log10(fMinHz);
    double logMax = std::log10(fMaxHz);
    int steps = static_cast<int>((logMax - logMin) * pointsPerDecade) + 1;
    for (int i = 0; i <= steps; ++i) {
        double f = std::pow(10.0, logMin + (logMax - logMin) * i / steps);
        if (line.covers(f)) {
            run.push_back({{"f", f}, {"v", line.level(f)}});
        } else if (!run.empty()) {
            runs.push_back(run);
            run = json::array();
        }
    }
    if (!run.empty()) {
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
    auto design = Hertz::design_line_filter(
        params.at("fSwHz").get<double>(), params.at("aReqCmDb").get<double>(),
        params.at("aReqDmDb").get<double>(), params.at("cYPerLineF").get<double>(), lDm,
        params.at("stages").get<int>(), params.at("lCmCandidatesH").get<std::vector<double>>(),
        params.at("cXCandidatesF").get<std::vector<double>>());

    json result{{"stages", design.stages},
                {"fDesignHz", design.fDesignHz},
                {"fCutoffTargetHz", design.fCutoffTargetHz},
                {"cYPerLineF", design.cYPerLineF},
                {"cYgF", design.cYgF},
                {"lCmRequiredH", design.lCmRequiredH},
                {"lCmSelectedH", design.lCmSelectedH},
                {"attenuationCmDb", design.attenuationCmDb},
                {"lDmH", design.lDmH},
                {"cXRequiredF", design.cXRequiredF},
                {"cXSelectedF", design.cXSelectedF},
                {"attenuationDmDb", design.attenuationDmDb}};

    if (params.contains("grid")) {
        const json& grid = params.at("grid");
        double vRms = grid.at("vRms").get<double>();
        double fGrid = grid.at("fHz").get<double>();
        // Worst case per ANP015: V +10 %, C +20 %.
        double cXTotal = design.cXSelectedF * design.stages * 1.2;
        double cY = design.cYPerLineF * 1.2;
        result["leakageCurrentA"] =
            Hertz::y_capacitor_leakage_current(vRms * 1.1, fGrid, cY, cY, cXTotal);
        double cTotal = design.cXSelectedF * design.stages;
        double rMax = Hertz::max_discharge_resistance(
            cTotal, vRms * std::numbers::sqrt2, grid.at("vSafe").get<double>(),
            grid.at("tDischargeS").get<double>());
        double rChosen = Hertz::round_down_to_series(rMax, Hertz::E24);
        result["dischargeResistorMaxOhm"] = rMax;
        result["dischargeResistorOhm"] = rChosen;
        result["dischargeResistorPowerW"] = Hertz::discharge_resistor_power(vRms, rChosen);
    }
    return result.dump();
});
}

// SPICE netlist of the designed filter between a noise port and the LISN, ready
// for Kirchhoff/ngspice/LTspice (.ac analysis of insertion loss).
std::string filter_spice_netlist_js(const std::string& designJson, const std::string& lisnKind)  {
    return guarded([&]() -> std::string {
    json design = json::parse(designJson);
    Hertz::Lisn lisn = lisnKind == "cispr25" ? Hertz::cispr25_lisn() : Hertz::cispr16_lisn();
    int stages = design.at("stages").get<int>();
    double lCm = design.at("lCmSelectedH").get<double>();
    double cX = design.at("cXSelectedF").get<double>();
    double cY = design.at("cYPerLineF").get<double>();
    double lDm = design.at("lDmH").get<double>();

    std::string netlist = "* Hertz line filter — ANP015 design\n";
    netlist += "* CM choke per stage: " + std::to_string(lCm) + " H (leakage " +
               std::to_string(lDm) + " H as DM element)\n";
    netlist += lisn.to_spice_subckt("LISN");
    char buffer[256];
    std::string nodeL = "line_src";
    std::string nodeN = "neut_src";
    for (int s = 1; s <= stages; ++s) {
        std::string outL = s == stages ? "line_out" : "line_" + std::to_string(s);
        std::string outN = s == stages ? "neut_out" : "neut_" + std::to_string(s);
        std::snprintf(buffer, sizeof(buffer),
                      "LcmL%d %s %s %.6g\nLcmN%d %s %s %.6g\nKcm%d LcmL%d LcmN%d 0.999\n", s,
                      nodeL.c_str(), outL.c_str(), lCm, s, nodeN.c_str(), outN.c_str(), lCm, s, s,
                      s);
        netlist += buffer;
        std::snprintf(buffer, sizeof(buffer), "Cx%d %s %s %.6g\n", s, outL.c_str(), outN.c_str(),
                      cX);
        netlist += buffer;
        std::snprintf(buffer, sizeof(buffer), "CyL%d %s 0 %.6g\nCyN%d %s 0 %.6g\n", s, outL.c_str(),
                      cY, s, outN.c_str(), cY);
        netlist += buffer;
        nodeL = outL;
        nodeN = outN;
    }
    netlist += "XlisnL line_out mains_l measL LISN\nXlisnN neut_out mains_n measN LISN\n";
    netlist += "Vmains mains_l mains_n DC 0\n";
    netlist += "* drive: replace with the converter noise source\n";
    netlist += "Vnoise line_src neut_src AC 1\n";
    netlist += ".ac dec 100 150k 30meg\n.end\n";
    return netlist;
});
}

std::string lisn_data_js(const std::string& kind, double fMinHz, double fMaxHz,
                         int pointsPerDecade)  {
    return guarded([&]() -> std::string {
    Hertz::Lisn lisn = kind == "cispr25" ? Hertz::cispr25_lisn() : Hertz::cispr16_lisn();
    if (!(0.0 < fMinHz && fMinHz < fMaxHz) || pointsPerDecade < 2) {
        throw std::invalid_argument("bad frequency range");
    }
    json freqs = json::array();
    json magnitude = json::array();
    json phase = json::array();
    double logMin = std::log10(fMinHz);
    double logMax = std::log10(fMaxHz);
    int steps = static_cast<int>((logMax - logMin) * pointsPerDecade);
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
            params.value("pointsPerDecade", 40));
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

}  // namespace

EMSCRIPTEN_BINDINGS(hertz) {
    emscripten::function("version", &engine_version);
    emscripten::function("parseSpectrumCsv", &parse_spectrum_csv_js);
    emscripten::function("limitAnalysis", &limit_analysis_js);
    emscripten::function("limitPolyline", &limit_polyline_js);
    emscripten::function("designFilter", &design_filter_js);
    emscripten::function("filterSpiceNetlist", &filter_spice_netlist_js);
    emscripten::function("lisnData", &lisn_data_js);
    emscripten::function("separateTraces", &separate_js);
    emscripten::function("measureWaveform", &measure_waveform_js);
    emscripten::function("detectComb", &detect_comb_js);
    emscripten::function("insertionLossCurves", &insertion_loss_curves_js);
    emscripten::function("inputFilterInteraction", &input_filter_interaction_js);
    emscripten::function("measuredIlCurves", &measured_il_curves_js);
}
