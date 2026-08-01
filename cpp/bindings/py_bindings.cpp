// PyHertz — pybind11 surface of the Hertz engine.
//
// Unlike the embind/WASM surface (wasm_bindings.cpp), which marshals everything
// through JSON strings for the browser, this module binds the C++ TYPES: the
// Python API mirrors src/hertz/*.py name for name (snake_case, string detector
// names, keyword defaults) so the reference implementation's own test suite can
// be pointed at the C++ engine and every divergence shows up as a failure.
//
// Every computation lives in the headers — this file only marshals.

#include <pybind11/complex.h>
#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <complex>
#include <limits>
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

namespace py = pybind11;

namespace {

// The Python API names detectors with strings ("peak" / "quasi_peak" /
// "average"); the C++ engine uses an enum. Convert at the boundary only.
Hertz::Detector detector_from_string(const std::string& name) {
    if (name == "peak") return Hertz::Detector::PEAK;
    if (name == "quasi_peak") return Hertz::Detector::QUASI_PEAK;
    if (name == "average") return Hertz::Detector::AVERAGE;
    throw std::invalid_argument("unknown detector " + name);
}

std::string detector_to_string(Hertz::Detector detector) {
    switch (detector) {
        case Hertz::Detector::PEAK: return "peak";
        case Hertz::Detector::QUASI_PEAK: return "quasi_peak";
        case Hertz::Detector::AVERAGE: return "average";
    }
    throw std::invalid_argument("unknown detector");
}

std::vector<double> series_values(const std::array<double, 6>& s) {
    return {s.begin(), s.end()};
}

}  // namespace

PYBIND11_MODULE(PyHertz, m) {
    m.doc() = "Hertz — conducted-EMI analysis and EMI-filter design (C++ engine)";
    m.attr("__version__") = "0.1.0";

    // ---------------------------------------------------------------- errors
    // Both derive from ValueError, matching the reference implementation
    // (OutsideCoverage(ValueError), TraceFormatError(ValueError)); the engine's
    // std::invalid_argument maps to ValueError through pybind11's own table.
    py::register_exception<Hertz::OutsideCoverage>(m, "OutsideCoverage", PyExc_ValueError);
    py::register_exception<Hertz::TraceFormatError>(m, "TraceFormatError", PyExc_ValueError);

    // ----------------------------------------------------------------- utils
    m.attr("E6") = py::cast(series_values(Hertz::E6));
    m.attr("E12") = py::cast(std::vector<double>(Hertz::E12.begin(), Hertz::E12.end()));
    m.attr("E24") = py::cast(std::vector<double>(Hertz::E24.begin(), Hertz::E24.end()));

    m.def("dbuv_from_vrms", &Hertz::dbuv_from_vrms, py::arg("v_rms"));
    m.def("vrms_from_dbuv", &Hertz::vrms_from_dbuv, py::arg("dbuv"));
    m.def("dbuv_from_dbm", &Hertz::dbuv_from_dbm, py::arg("dbm"), py::arg("z0_ohm") = 50.0);
    m.def("round_up_to", &Hertz::round_up_to, py::arg("value"), py::arg("candidates"));
    m.def("round_down_to", &Hertz::round_down_to, py::arg("value"), py::arg("candidates"));
    m.def("round_up_to_series",
          [](double value, const std::vector<double>& series) {
              return Hertz::round_up_to_series(value, series);
          },
          py::arg("value"), py::arg("series") = series_values(Hertz::E6));
    m.def("round_down_to_series",
          [](double value, const std::vector<double>& series) {
              return Hertz::round_down_to_series(value, series);
          },
          py::arg("value"), py::arg("series") = series_values(Hertz::E6));

    // ------------------------------------------------------------ separation
    m.def("separate",
          [](const std::vector<double>& v_line, const std::vector<double>& v_neutral) {
              auto [cm, dm] = Hertz::separate(v_line, v_neutral);
              return py::make_tuple(cm, dm);
          },
          py::arg("v_line"), py::arg("v_neutral"),
          "(common_mode, differential_mode) from the two line quantities.");
    // Complex spectra are the other documented input (Separation.hpp): the
    // engine template covers them, so the binding must too — silently taking
    // the real part of a complex spectrum would discard the phase the whole
    // separation depends on.
    m.def("separate",
          [](const std::vector<std::complex<double>>& v_line,
             const std::vector<std::complex<double>>& v_neutral) {
              auto [cm, dm] = Hertz::separate(v_line, v_neutral);
              return py::make_tuple(cm, dm);
          },
          py::arg("v_line"), py::arg("v_neutral"),
          "(common_mode, differential_mode) from two complex spectra.");

    // ---------------------------------------------------------------- limits
    py::class_<Hertz::LimitSegment>(m, "LimitSegment")
        .def(py::init<double, double, double, double>(), py::arg("f_start_hz"),
             py::arg("f_stop_hz"), py::arg("level_start_dbuv"), py::arg("level_stop_dbuv"))
        .def_readonly("f_start_hz", &Hertz::LimitSegment::fStartHz)
        .def_readonly("f_stop_hz", &Hertz::LimitSegment::fStopHz)
        .def_readonly("level_start_dbuv", &Hertz::LimitSegment::levelStartDbuv)
        .def_readonly("level_stop_dbuv", &Hertz::LimitSegment::levelStopDbuv)
        .def("level", &Hertz::LimitSegment::level, py::arg("f_hz"))
        .def("__repr__", [](const Hertz::LimitSegment& s) {
            return "LimitSegment(" + std::to_string(s.fStartHz) + ", " +
                   std::to_string(s.fStopHz) + ", " + std::to_string(s.levelStartDbuv) + ", " +
                   std::to_string(s.levelStopDbuv) + ")";
        });

    py::class_<Hertz::LimitLine>(m, "LimitLine")
        .def(py::init([](std::string name, const std::string& detector,
                         std::vector<Hertz::LimitSegment> segments) {
                 return Hertz::LimitLine(std::move(name), detector_from_string(detector),
                                         std::move(segments));
             }),
             py::arg("name"), py::arg("detector"), py::arg("segments"))
        .def_property_readonly("name", &Hertz::LimitLine::name)
        .def_property_readonly(
            "detector", [](const Hertz::LimitLine& l) { return detector_to_string(l.detector()); })
        .def_property_readonly("segments", &Hertz::LimitLine::segments)
        .def("covers", &Hertz::LimitLine::covers, py::arg("f_hz"))
        .def("level", &Hertz::LimitLine::level, py::arg("f_hz"))
        .def("margin", &Hertz::LimitLine::margin, py::arg("f_hz"), py::arg("measured_dbuv"))
        // (mask, levels) over a frequency vector; levels is NaN where the line
        // does not apply. Pure marshalling over covers()/level() so the
        // vectorized path cannot drift from the scalar one.
        .def("levels_where_covered",
             [](const Hertz::LimitLine& line, const std::vector<double>& f_hz) {
                 std::vector<bool> mask;
                 std::vector<double> levels;
                 mask.reserve(f_hz.size());
                 levels.reserve(f_hz.size());
                 for (double f : f_hz) {
                     bool covered = line.covers(f);
                     mask.push_back(covered);
                     levels.push_back(covered ? line.level(f)
                                              : std::numeric_limits<double>::quiet_NaN());
                 }
                 return py::make_tuple(mask, levels);
             },
             py::arg("f_hz"))
        .def("__repr__",
             [](const Hertz::LimitLine& l) { return "LimitLine(" + l.name() + ")"; });

    // The four CISPR 32 mains lines are singletons in the engine; hand out
    // references so `line is CISPR32_CLASS_B_MAINS_QP` holds, as in Python.
    m.attr("CISPR32_CLASS_B_MAINS_QP") =
        py::cast(Hertz::cispr32_class_b_mains_qp(), py::return_value_policy::reference);
    m.attr("CISPR32_CLASS_B_MAINS_AVG") =
        py::cast(Hertz::cispr32_class_b_mains_avg(), py::return_value_policy::reference);
    m.attr("CISPR32_CLASS_A_MAINS_QP") =
        py::cast(Hertz::cispr32_class_a_mains_qp(), py::return_value_policy::reference);
    m.attr("CISPR32_CLASS_A_MAINS_AVG") =
        py::cast(Hertz::cispr32_class_a_mains_avg(), py::return_value_policy::reference);

    m.def("cispr25_conducted_voltage",
          [](int emission_class, const std::string& detector) {
              return Hertz::cispr25_conducted_voltage(emission_class,
                                                      detector_from_string(detector));
          },
          py::arg("emission_class"), py::arg("detector"));
    m.def("cispr32_radiated",
          [](const std::string& emission_class, double distance_m) {
              if (emission_class.size() != 1) {
                  throw std::invalid_argument("CISPR 32 radiated class must be A or B");
              }
              return Hertz::cispr32_radiated(emission_class[0], distance_m);
          },
          py::arg("emission_class"), py::arg("distance_m"));

    m.def("unswept_regions",
          [](const Hertz::LimitLine& line, double f_lo_hz, double f_hi_hz) {
              return Hertz::unswept_regions(line, f_lo_hz, f_hi_hz);
          },
          py::arg("line"), py::arg("f_lo_hz"), py::arg("f_hi_hz"));
    m.def("unswept_regions_sampled",
          [](const Hertz::LimitLine& line, std::vector<double> freqs_hz) {
              return Hertz::unswept_regions(line, std::move(freqs_hz));
          },
          py::arg("line"), py::arg("freqs_hz"));
    m.def("limit_polyline_runs", &Hertz::limit_polyline_runs, py::arg("line"), py::arg("f_min_hz"),
          py::arg("f_max_hz"), py::arg("points_per_decade"));
    m.def("cispr_rbw_hz", &Hertz::cispr_rbw_hz, py::arg("f_hz"));

    m.attr("UNSWEPT_GAP_DECADES") = Hertz::UNSWEPT_GAP_DECADES;
    m.attr("UNSWEPT_RELATIVE_FACTOR") = Hertz::UNSWEPT_RELATIVE_FACTOR;
    m.attr("UNSWEPT_LOCAL_WINDOW") = Hertz::UNSWEPT_LOCAL_WINDOW;
    m.attr("UNSWEPT_SEGMENT_SWALLOW") = Hertz::UNSWEPT_SEGMENT_SWALLOW;
    m.attr("UNSWEPT_MIN_HOLE_RBW") = Hertz::UNSWEPT_MIN_HOLE_RBW;

    // ------------------------------------------------------------------ LISN
    py::class_<Hertz::Lisn>(m, "Lisn")
        .def(py::init<std::string, double, double, double>(), py::arg("name"),
             py::arg("inductance_h"), py::arg("coupling_capacitance_f"),
             py::arg("measuring_impedance_ohm") = 50.0)
        .def_readonly("name", &Hertz::Lisn::name)
        .def_readonly("inductance_h", &Hertz::Lisn::inductanceH)
        .def_readonly("coupling_capacitance_f", &Hertz::Lisn::couplingCapacitanceF)
        .def_readonly("measuring_impedance_ohm", &Hertz::Lisn::measuringImpedanceOhm)
        .def("measuring_branch_impedance", &Hertz::Lisn::measuring_branch_impedance,
             py::arg("f_hz"))
        .def("eut_impedance", &Hertz::Lisn::eut_impedance, py::arg("f_hz"))
        // Vector overloads (tried after the scalar one) so a caller can sweep
        // an array exactly as the reference API does — same scalar engine call
        // per point, no separate vectorized formula.
        .def("measuring_branch_impedance",
             [](const Hertz::Lisn& lisn, const std::vector<double>& f_hz) {
                 std::vector<std::complex<double>> out;
                 out.reserve(f_hz.size());
                 for (double f : f_hz) out.push_back(lisn.measuring_branch_impedance(f));
                 return out;
             },
             py::arg("f_hz"))
        .def("eut_impedance",
             [](const Hertz::Lisn& lisn, const std::vector<double>& f_hz) {
                 std::vector<std::complex<double>> out;
                 out.reserve(f_hz.size());
                 for (double f : f_hz) out.push_back(lisn.eut_impedance(f));
                 return out;
             },
             py::arg("f_hz"))
        .def("to_spice_subckt",
             [](const Hertz::Lisn& lisn, std::optional<std::string> name, bool terminated) {
                 return lisn.to_spice_subckt(name.value_or(""), terminated);
             },
             py::arg("name") = py::none(), py::arg("terminated") = true)
        .def("__repr__", [](const Hertz::Lisn& l) { return "Lisn(" + l.name + ")"; });

    m.def("cispr16_lisn", &Hertz::cispr16_lisn);
    m.def("cispr25_lisn", &Hertz::cispr25_lisn);

    // -------------------------------------------------------------- detector
    py::class_<Hertz::CisprBand>(m, "CisprBand")
        .def_property_readonly("name", [](const Hertz::CisprBand& b) { return std::string(b.name); })
        .def_readonly("f_min_hz", &Hertz::CisprBand::fMinHz)
        .def_readonly("f_max_hz", &Hertz::CisprBand::fMaxHz)
        .def_readonly("rbw_6db_hz", &Hertz::CisprBand::rbw6dbHz)
        .def_readonly("tau_charge_s", &Hertz::CisprBand::tauChargeS)
        .def_readonly("tau_discharge_s", &Hertz::CisprBand::tauDischargeS)
        .def_readonly("tau_meter_s", &Hertz::CisprBand::tauMeterS)
        .def("__repr__", [](const Hertz::CisprBand& b) {
            return std::string("CisprBand(") + b.name + ")";
        });

    // Reference casts so band_for_frequency(f) IS BAND_B, as in the reference
    // implementation (the bands are engine singletons).
    m.attr("BAND_A") = py::cast(Hertz::BAND_A, py::return_value_policy::reference);
    m.attr("BAND_B") = py::cast(Hertz::BAND_B, py::return_value_policy::reference);
    m.attr("BAND_C") = py::cast(Hertz::BAND_C, py::return_value_policy::reference);
    m.attr("BAND_D") = py::cast(Hertz::BAND_D, py::return_value_policy::reference);
    m.def("band_for_frequency", &Hertz::band_for_frequency, py::arg("f_hz"),
          py::return_value_policy::reference);

    py::class_<Hertz::ReceiverReading>(m, "ReceiverReading")
        .def_readonly("frequencies_hz", &Hertz::ReceiverReading::frequenciesHz)
        .def_readonly("peak_dbuv", &Hertz::ReceiverReading::peakDbuv)
        .def_readonly("quasi_peak_dbuv", &Hertz::ReceiverReading::quasiPeakDbuv)
        .def_readonly("average_dbuv", &Hertz::ReceiverReading::averageDbuv);

    m.def("measure", &Hertz::measure, py::arg("x"), py::arg("fs_hz"), py::arg("band"),
          py::arg("overlap") = 0.9,
          "EMI-receiver readings (peak / quasi-peak / average per FFT bin, dBuV).");

    m.def("stft_envelope",
          [](const std::vector<double>& x, double fs_hz, const Hertz::CisprBand& band,
             double overlap) {
              // The plan is rebuilt here only to recover the frame geometry
              // (winLen/hop) the reference API reports as `times`; it validates
              // exactly as the engine's own call does.
              Hertz::detail::StftPlan plan(fs_hz, band, overlap, x.size());
              std::vector<double> freqs;
              auto envelope = Hertz::stft_envelope(x, fs_hz, band, &freqs, overlap);
              const size_t half = (plan.winLen - 1) / 2;
              std::vector<double> times;
              times.reserve(envelope.size());
              for (size_t i = 0; i < envelope.size(); ++i) {
                  times.push_back(static_cast<double>(i * plan.hop + half) / fs_hz);
              }
              py::array_t<double> matrix({static_cast<py::ssize_t>(envelope.size()),
                                          static_cast<py::ssize_t>(plan.bins)});
              auto view = matrix.mutable_unchecked<2>();
              for (size_t i = 0; i < envelope.size(); ++i) {
                  for (size_t k = 0; k < plan.bins; ++k) {
                      view(static_cast<py::ssize_t>(i), static_cast<py::ssize_t>(k)) =
                          envelope[i][k];
                  }
              }
              return py::make_tuple(freqs, times, matrix);
          },
          py::arg("x"), py::arg("fs_hz"), py::arg("band"), py::arg("overlap") = 0.9,
          "(freqs_hz, times_s, envelope[n_frames, n_bins]) amplitude envelope.");

    m.attr("SETTLE_METER_TAUS") = Hertz::SETTLE_METER_TAUS;

    // --------------------------------------------------------- filter design
    m.attr("MIN_MEASURED_FREQUENCY_HZ") = Hertz::MIN_MEASURED_FREQUENCY_HZ;
    m.def("design_frequency", &Hertz::design_frequency, py::arg("f_sw_hz"));
    m.def("required_attenuation_db", &Hertz::required_attenuation_db, py::arg("measured_dbuv"),
          py::arg("limit_dbuv"), py::arg("margin_db") = 10.0);
    m.def("cutoff_frequency", &Hertz::cutoff_frequency, py::arg("f_design_hz"), py::arg("a_req_db"),
          py::arg("stages"));
    m.def("resonant_cutoff", &Hertz::resonant_cutoff, py::arg("inductance_h"),
          py::arg("capacitance_f"));
    m.def("x_capacitor_dm_factor", &Hertz::x_capacitor_dm_factor, py::arg("n_lines"));
    m.def("cm_inductance", &Hertz::cm_inductance, py::arg("f_cutoff_hz"), py::arg("c_yg_f"));
    m.def("dm_capacitance", &Hertz::dm_capacitance, py::arg("f_cutoff_hz"), py::arg("l_dm_h"));
    m.def("leakage_inductance_from_impedance", &Hertz::leakage_inductance_from_impedance,
          py::arg("z_mag_ohm"), py::arg("f_hz"));
    m.def("achieved_attenuation_db", &Hertz::achieved_attenuation_db, py::arg("f_design_hz"),
          py::arg("inductance_h"), py::arg("capacitance_f"), py::arg("stages"));

    py::class_<Hertz::GoverningRequirement>(m, "GoverningRequirement")
        .def_readonly("attenuation_db", &Hertz::GoverningRequirement::attenuationDb)
        .def_readonly("frequency_hz", &Hertz::GoverningRequirement::frequencyHz);
    m.def("governing_requirement", &Hertz::governing_requirement, py::arg("frequencies_hz"),
          py::arg("a_req_db"));

    py::class_<Hertz::LineFilterDesign>(m, "LineFilterDesign")
        .def_readonly("stages", &Hertz::LineFilterDesign::stages)
        .def_readonly("f_design_cm_hz", &Hertz::LineFilterDesign::fDesignCmHz)
        .def_readonly("f_design_dm_hz", &Hertz::LineFilterDesign::fDesignDmHz)
        .def_readonly("f_cutoff_cm_hz", &Hertz::LineFilterDesign::fCutoffCmHz)
        .def_readonly("f_cutoff_dm_hz", &Hertz::LineFilterDesign::fCutoffDmHz)
        .def_readonly("c_y_per_line_f", &Hertz::LineFilterDesign::cYPerLineF)
        .def_readonly("c_yg_f", &Hertz::LineFilterDesign::cYgF)
        .def_readonly("l_cm_required_h", &Hertz::LineFilterDesign::lCmRequiredH)
        .def_readonly("l_cm_selected_h", &Hertz::LineFilterDesign::lCmSelectedH)
        .def_readonly("attenuation_cm_db", &Hertz::LineFilterDesign::attenuationCmDb)
        .def_readonly("l_dm_h", &Hertz::LineFilterDesign::lDmH)
        .def_readonly("c_x_required_f", &Hertz::LineFilterDesign::cXRequiredF)
        .def_readonly("c_x_selected_f", &Hertz::LineFilterDesign::cXSelectedF)
        .def_readonly("attenuation_dm_db", &Hertz::LineFilterDesign::attenuationDmDb)
        .def_readonly("n_lines", &Hertz::LineFilterDesign::nLines)
        .def_readonly("c_x_dm_factor", &Hertz::LineFilterDesign::cXDmFactor)
        .def_readonly("l_cm_floor_from_leakage", &Hertz::LineFilterDesign::lCmFloorFromLeakage);

    m.def("design_line_filter", &Hertz::design_line_filter, py::arg("f_sw_hz"),
          py::arg("a_req_cm_db"), py::arg("a_req_dm_db"), py::arg("c_y_per_line_f"),
          py::arg("l_dm_h"), py::arg("stages"), py::arg("l_cm_candidates"),
          py::arg("c_x_candidates"), py::arg("f_design_cm_hz") = py::none(),
          py::arg("f_design_dm_hz") = py::none(), py::arg("n_lines") = 2);

    m.def("y_capacitor_leakage_current", &Hertz::y_capacitor_leakage_current,
          py::arg("v_grid_rms"), py::arg("f_grid_hz"), py::arg("c_y_line_f"),
          py::arg("c_y_neutral_f"), py::arg("c_x_total_f"));
    m.def("max_discharge_resistance", &Hertz::max_discharge_resistance, py::arg("c_total_f"),
          py::arg("v_start"), py::arg("v_safe"), py::arg("t_max_s"));
    m.def("discharge_resistor_power", &Hertz::discharge_resistor_power, py::arg("v_grid_rms"),
          py::arg("resistance_ohm"));

    py::class_<Hertz::FilterInteraction>(m, "FilterInteraction")
        .def_readonly("resonance_hz", &Hertz::FilterInteraction::resonanceHz)
        .def_readonly("characteristic_impedance_ohm",
                      &Hertz::FilterInteraction::characteristicImpedanceOhm)
        .def_readonly("converter_input_impedance_ohm",
                      &Hertz::FilterInteraction::converterInputImpedanceOhm)
        .def_readonly("margin_db", &Hertz::FilterInteraction::marginDb)
        .def_readonly("damping_resistor_ohm", &Hertz::FilterInteraction::dampingResistorOhm)
        .def_readonly("damping_capacitor_min_f", &Hertz::FilterInteraction::dampingCapacitorMinF)
        .def_readonly("damping_capacitor_max_f", &Hertz::FilterInteraction::dampingCapacitorMaxF);
    m.def("input_filter_interaction", &Hertz::input_filter_interaction, py::arg("inductance_h"),
          py::arg("capacitance_f"), py::arg("v_in_min"), py::arg("p_in"));

    // --------------------------------------------------------------- network
    py::class_<Hertz::Abcd>(m, "Abcd")
        .def(py::init([](Hertz::Complex a, Hertz::Complex b, Hertz::Complex c, Hertz::Complex d) {
                 return Hertz::Abcd{a, b, c, d};
             }),
             py::arg("a") = Hertz::Complex(1.0, 0.0), py::arg("b") = Hertz::Complex(0.0, 0.0),
             py::arg("c") = Hertz::Complex(0.0, 0.0), py::arg("d") = Hertz::Complex(1.0, 0.0))
        .def_readonly("a", &Hertz::Abcd::a)
        .def_readonly("b", &Hertz::Abcd::b)
        .def_readonly("c", &Hertz::Abcd::c)
        .def_readonly("d", &Hertz::Abcd::d);

    m.def("series_impedance", &Hertz::series_impedance, py::arg("z"));
    m.def("shunt_admittance", &Hertz::shunt_admittance, py::arg("y"));
    m.def("cascade", &Hertz::cascade, py::arg("first"), py::arg("second"));
    m.def("insertion_loss_db", &Hertz::insertion_loss_db, py::arg("network"), py::arg("z_source"),
          py::arg("z_load"));
    m.def("lc_filter_abcd", &Hertz::lc_filter_abcd, py::arg("f_hz"), py::arg("inductance_h"),
          py::arg("capacitance_f"), py::arg("stages"), py::arg("cap_esl_h") = 0.0,
          py::arg("cap_esr_ohm") = 0.0);
    m.def("insertion_loss_worst_case_db", &Hertz::insertion_loss_worst_case_db, py::arg("network"));

    py::class_<Hertz::InsertionLossCurves>(m, "InsertionLossCurves")
        .def_readonly("frequencies_hz", &Hertz::InsertionLossCurves::frequenciesHz)
        .def_readonly("standard_db", &Hertz::InsertionLossCurves::standardDb)
        .def_readonly("worst_case_db", &Hertz::InsertionLossCurves::worstCaseDb);

    m.def("insertion_loss_curves", &Hertz::insertion_loss_curves, py::arg("inductance_h"),
          py::arg("capacitance_f"), py::arg("stages"), py::arg("reference_impedance_ohm"),
          py::arg("f_min_hz"), py::arg("f_max_hz"), py::arg("points_per_decade") = 40,
          py::arg("cap_esl_h") = 0.0, py::arg("cap_esr_ohm") = 0.0);
    m.def("tabulated_series_il_curves", &Hertz::tabulated_series_il_curves, py::arg("freqs_hz"),
          py::arg("z_real_ohm"), py::arg("z_imag_ohm"), py::arg("c_shunt_f"), py::arg("stages"),
          py::arg("reference_impedance_ohm"));

    // ------------------------------------------------------------------ comb
    py::class_<Hertz::MatchedHarmonic>(m, "MatchedHarmonic")
        .def_readonly("order", &Hertz::MatchedHarmonic::order)
        .def_readonly("frequency_hz", &Hertz::MatchedHarmonic::frequencyHz)
        .def_readonly("level_dbuv", &Hertz::MatchedHarmonic::levelDbuv)
        .def("__repr__", [](const Hertz::MatchedHarmonic& h) {
            return "MatchedHarmonic(order=" + std::to_string(h.order) + ", frequency_hz=" +
                   std::to_string(h.frequencyHz) + ")";
        });

    py::class_<Hertz::CombResult>(m, "CombResult")
        .def_readonly("found", &Hertz::CombResult::found)
        // None when nothing was found — a guessed f_sw is exactly what this
        // detector refuses to produce (the reference API returns None too).
        .def_property_readonly("f_sw_hz",
                               [](const Hertz::CombResult& r) {
                                   return r.found ? py::cast(r.fSwHz) : py::none();
                               })
        .def_readonly("confidence", &Hertz::CombResult::confidence)
        .def_readonly("coverage", &Hertz::CombResult::coverage)
        .def_readonly("harmonics", &Hertz::CombResult::harmonics)
        .def_readonly("residual_peaks", &Hertz::CombResult::residualPeaks);

    m.def("detect_comb", &Hertz::detect_comb, py::arg("freqs_hz"), py::arg("levels_dbuv"),
          py::arg("f_sw_min_hz") = 20e3, py::arg("f_sw_max_hz") = 5e6);

    m.attr("COMB_PROMINENCE_THRESHOLD_DB") = Hertz::COMB_PROMINENCE_THRESHOLD_DB;
    m.attr("COMB_MIN_MATCHED_HARMONICS") = Hertz::COMB_MIN_MATCHED_HARMONICS;
    m.attr("COMB_CONFIDENCE_THRESHOLD") = Hertz::COMB_CONFIDENCE_THRESHOLD;

    // -------------------------------------------------------------- radiated
    m.attr("RADIATED_MODEL_UNCERTAINTY_DB") = Hertz::RADIATED_MODEL_UNCERTAINTY_DB;
    m.attr("RADIATED_TARGET_MARGIN_DB") = Hertz::RADIATED_TARGET_MARGIN_DB;
    m.def("radiated_efield_dbuvm", &Hertz::radiated_efield_dbuvm, py::arg("frequencies_hz"),
          py::arg("cm_current_dbua"), py::arg("cable_length_m"), py::arg("distance_m"));
    m.def("radiated_cm_attenuation_target_db", &Hertz::radiated_cm_attenuation_target_db,
          py::arg("frequencies_hz"), py::arg("cm_current_dbua"), py::arg("cable_length_m"),
          py::arg("distance_m"), py::arg("limit_dbuvm"),
          py::arg("margin_db") = Hertz::RADIATED_TARGET_MARGIN_DB);

    // ------------------------------------------------------- cable mitigation
    py::class_<Hertz::FerritePart>(m, "FerritePart")
        // phase_rad is optional: empty means the catalog carries |Z| only and
        // the engine treats the part as resistive.
        .def(py::init([](std::string name, std::vector<double> frequencies_hz,
                         std::vector<double> z_ohm, std::vector<double> phase_rad) {
                 return Hertz::FerritePart{std::move(name), std::move(frequencies_hz),
                                           std::move(z_ohm), std::move(phase_rad)};
             }),
             py::arg("name"), py::arg("frequencies_hz"), py::arg("z_ohm"),
             py::arg("phase_rad") = std::vector<double>{})
        .def_readonly("name", &Hertz::FerritePart::name)
        .def_readonly("frequencies_hz", &Hertz::FerritePart::frequenciesHz)
        .def_readonly("z_ohm", &Hertz::FerritePart::zOhm)
        .def_readonly("phase_rad", &Hertz::FerritePart::phaseRad);

    py::class_<Hertz::MitigationChoice>(m, "MitigationChoice")
        .def_readonly("part_name", &Hertz::MitigationChoice::partName)
        .def_readonly("turns", &Hertz::MitigationChoice::turns)
        .def_readonly("worst_margin_db", &Hertz::MitigationChoice::worstMarginDb)
        .def_readonly("meets_target", &Hertz::MitigationChoice::meetsTarget)
        .def_readonly("insertion_loss_db", &Hertz::MitigationChoice::insertionLossDb);

    m.def("ferrite_impedance_at", &Hertz::ferrite_impedance_at, py::arg("part"), py::arg("freqs"));
    m.def("ferrite_insertion_loss_db", &Hertz::ferrite_insertion_loss_db, py::arg("part"),
          py::arg("turns"), py::arg("cm_reference_ohm"), py::arg("frequencies_hz"));
    m.def("select_cable_mitigation", &Hertz::select_cable_mitigation, py::arg("frequencies_hz"),
          py::arg("required_attenuation_db"), py::arg("parts"), py::arg("cm_reference_ohm"),
          py::arg("max_turns") = 1);

    // ----------------------------------------------------------- spice decks
    m.def("filter_spice_deck", &Hertz::filter_spice_deck, py::arg("stages"), py::arg("l_cm_h"),
          py::arg("c_x_f"), py::arg("c_y_per_line_f"), py::arg("l_dm_h"), py::arg("lisn"),
          py::arg("mode"), py::arg("n_lines") = 2);
    m.def("lisn_reference_deck", &Hertz::lisn_reference_deck, py::arg("lisn"), py::arg("mode"),
          py::arg("n_lines") = 2);
    m.def("deck_abcd_il", &Hertz::deck_abcd_il, py::arg("lisn"), py::arg("mode"),
          py::arg("n_lines"), py::arg("stages"), py::arg("l_cm_h"), py::arg("c_y_per_line_f"),
          py::arg("l_dm_h"), py::arg("c_x_f"), py::arg("c_x_dm_factor"), py::arg("freqs_hz"));

    // ---------------------------------------------------------------- traces
    py::class_<Hertz::SpectrumTrace>(m, "SpectrumTrace")
        .def_readonly("frequencies_hz", &Hertz::SpectrumTrace::frequenciesHz)
        .def_readonly("levels_dbuv", &Hertz::SpectrumTrace::levelsDbuv)
        .def_readonly("level_unit", &Hertz::SpectrumTrace::levelUnit);

    py::class_<Hertz::SpectrumCsvColumns>(m, "SpectrumCsvColumns")
        .def_readonly("count", &Hertz::SpectrumCsvColumns::count)
        .def_readonly("frequency_column", &Hertz::SpectrumCsvColumns::frequencyColumn)
        .def_readonly("names", &Hertz::SpectrumCsvColumns::names);

    m.def("parse_spectrum_csv", &Hertz::parse_spectrum_csv, py::arg("content"),
          py::arg("freq_unit") = py::none(), py::arg("level_unit") = py::none(),
          py::arg("z0_ohm") = 50.0, py::arg("level_column") = py::none());
    m.def("spectrum_csv_columns", &Hertz::spectrum_csv_columns, py::arg("content"));

    // ------------------------------------------------- the filter BLOCK (S3/S4)
    py::class_<Hertz::layout::GeneratedBoard>(m, "GeneratedBoard")
        .def_readonly("kicad_pcb", &Hertz::layout::GeneratedBoard::kicadPcb)
        .def_readonly("board_w_mm", &Hertz::layout::GeneratedBoard::boardWmm)
        .def_readonly("board_h_mm", &Hertz::layout::GeneratedBoard::boardHmm)
        .def_readonly("min_ln_gap_mm", &Hertz::layout::GeneratedBoard::minLNGapmm)
        .def_readonly("min_pe_gap_mm", &Hertz::layout::GeneratedBoard::minPEGapmm)
        .def_readonly("parts", &Hertz::layout::GeneratedBoard::parts);
    py::class_<Hertz::layout::LayoutParasitics>(m, "LayoutParasitics")
        .def_readonly("m_dm_nh", &Hertz::layout::LayoutParasitics::mDmNh)
        .def_readonly("x_conn_nh", &Hertz::layout::LayoutParasitics::xConnNh)
        .def_readonly("y_conn_nh", &Hertz::layout::LayoutParasitics::yConnNh)
        .def_readonly("pe_spine_nh", &Hertz::layout::LayoutParasitics::peSpineNh);
    py::class_<Hertz::layout::FilterBlock>(m, "FilterBlock")
        .def_readonly("design", &Hertz::layout::FilterBlock::design)
        .def_readonly("board", &Hertz::layout::FilterBlock::board)
        .def_readonly("parasitics", &Hertz::layout::FilterBlock::par)
        .def_readonly("layout_atten_cm_db", &Hertz::layout::FilterBlock::layoutAttenCmDb)
        .def_readonly("layout_atten_dm_db", &Hertz::layout::FilterBlock::layoutAttenDmDb)
        .def_readonly("meets", &Hertz::layout::FilterBlock::meets)
        .def_readonly("escalated_stages", &Hertz::layout::FilterBlock::escalatedStages)
        .def_readonly("iterations", &Hertz::layout::FilterBlock::iterations);
    m.def("optimize_filter_block", &Hertz::layout::optimize_filter_block,
          py::arg("f_sw_hz"), py::arg("a_req_cm_db"), py::arg("a_req_dm_db"),
          py::arg("c_y_per_line_f"), py::arg("l_dm_h"), py::arg("rated_current_a"),
          py::arg("l_cm_candidates_h"), py::arg("c_x_candidates_f"),
          py::arg("max_stages") = 2, py::arg("cap_esl_h") = 5e-9,
          py::arg("cap_esr_ohm") = 0.02);
    m.def("block_spice_subckt", &Hertz::layout::block_spice_subckt,
          py::arg("design"), py::arg("parasitics"), py::arg("cap_esl_h") = 5e-9,
          py::arg("cap_esr_ohm") = 0.02);
    m.def("touchstone_s2p", &Hertz::layout::touchstone_s2p, py::arg("design"),
          py::arg("parasitics"), py::arg("mode"), py::arg("f1_hz") = 150e3,
          py::arg("f2_hz") = 30e6, py::arg("points_per_decade") = 20,
          py::arg("cap_esl_h") = 5e-9, py::arg("cap_esr_ohm") = 0.02);
    m.def("block_bom_csv", &Hertz::layout::block_bom_csv, py::arg("design"));
}
