// Golden vectors are shared with the Python reference implementation
// (../../tests): the ANP015 worked example, CISPR 32 breakpoints, and the
// CW / pulsed receiver-calibration cases.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

#include "hertz/CableMitigation.hpp"
#include "hertz/Detector.hpp"
#include "hertz/FilterDesign.hpp"
#include "hertz/Limits.hpp"
#include "hertz/Network.hpp"
#include "hertz/Radiated.hpp"
#include "hertz/Lisn.hpp"
#include "hertz/Separation.hpp"
#include "hertz/Traces.hpp"
#include "hertz/Utils.hpp"

using Catch::Approx;

TEST_CASE("CISPR 32 Class B QP breakpoints", "[limits]") {
    const auto& line = Hertz::cispr32_class_b_mains_qp();
    CHECK(line.level(150e3) == Approx(66.0));
    CHECK(line.level(500e3) == Approx(56.0));
    CHECK(line.level(4.9e6) == Approx(56.0));
    CHECK(line.level(5.1e6) == Approx(60.0));
    CHECK(line.level(30e6) == Approx(60.0));
}

TEST_CASE("CISPR 32 log-frequency interpolation", "[limits]") {
    double fMid = std::sqrt(150e3 * 500e3);
    CHECK(Hertz::cispr32_class_b_mains_qp().level(fMid) == Approx(61.0));
    CHECK(Hertz::cispr32_class_b_mains_avg().level(fMid) == Approx(51.0));
}

TEST_CASE("Outside coverage throws", "[limits]") {
    CHECK_THROWS_AS(Hertz::cispr32_class_b_mains_qp().level(100e3), Hertz::OutsideCoverage);
    CHECK_THROWS_AS(Hertz::cispr32_class_b_mains_qp().level(50e6), Hertz::OutsideCoverage);
}

TEST_CASE("CISPR 25 class 5 LW band and detector offsets", "[limits]") {
    auto qp = Hertz::cispr25_conducted_voltage(5, Hertz::Detector::QUASI_PEAK);
    CHECK(qp.level(200e3) == Approx(57.0));
    CHECK(Hertz::cispr25_conducted_voltage(5, Hertz::Detector::PEAK).level(200e3) == Approx(70.0));
    CHECK(Hertz::cispr25_conducted_voltage(5, Hertz::Detector::AVERAGE).level(200e3) == Approx(50.0));
    CHECK_THROWS_AS(qp.level(400e3), Hertz::OutsideCoverage);  // gap between LW and MW

    // Table 4 class steps are band-dependent (F8-2, round 8): 10 dB LW, 8 dB
    // MW, 6 dB from SW up. The old flat +10/+20 was up to 8 dB too permissive.
    auto qpLevel = [](int cls, double f) {
        return Hertz::cispr25_conducted_voltage(cls, Hertz::Detector::QUASI_PEAK).level(f);
    };
    CHECK(qpLevel(4, 200e3) == Approx(67.0));   // LW: 57 + 10
    CHECK(qpLevel(3, 200e3) == Approx(77.0));
    CHECK(qpLevel(4, 1e6) == Approx(49.0));     // MW: 41 + 8
    CHECK(qpLevel(3, 1e6) == Approx(57.0));
    CHECK(qpLevel(4, 6e6) == Approx(46.0));     // SW: 40 + 6
    CHECK(qpLevel(3, 6e6) == Approx(52.0));
    CHECK(qpLevel(3, 27e6) == Approx(43.0));    // CB: 31 + 12
    CHECK(qpLevel(3, 90e6) == Approx(37.0));    // FM: 25 + 12

    // F8-4: the 30-54 and 68-87 MHz voltage-method bands exist in Table 4
    CHECK(qpLevel(5, 40e6) == Approx(31.0));
    CHECK(qpLevel(5, 70e6) == Approx(25.0));
    CHECK(qpLevel(5, 80e6) == Approx(25.0));    // 68-87 mobile == 76-108 FM values
    CHECK_THROWS_AS(qpLevel(5, 60e6), Hertz::OutsideCoverage);  // 54-68 MHz is a real gap
    CHECK_THROWS_AS(qpLevel(5, 29e6), Hertz::OutsideCoverage);  // 28-30 MHz is a real gap
}

TEST_CASE("Lower limit applies at a segment boundary (F9-1, round 9)", "[limits]") {
    // CISPR transition rule: at exactly 500 kHz Class A is 73 dBuV, not 79.
    CHECK(Hertz::cispr32_class_a_mains_qp().level(500e3) == Approx(73.0));
    CHECK(Hertz::cispr32_class_a_mains_avg().level(500e3) == Approx(60.0));
    CHECK(Hertz::cispr32_class_b_mains_qp().level(500e3) == Approx(56.0));
    CHECK(Hertz::cispr32_class_b_mains_qp().level(5e6) == Approx(56.0));  // lower of 56/60
}

TEST_CASE("Limit polyline emits exact band edges per segment (F9-2, round 9)", "[limits]") {
    // Decade sampling left CISPR 25's 5.9-6.2 MHz band with one invisible
    // point; every segment must now be a run with its true endpoints.
    auto runs = Hertz::limit_polyline_runs(
        Hertz::cispr25_conducted_voltage(5, Hertz::Detector::QUASI_PEAK), 150e3, 108e6, 40);
    REQUIRE(runs.size() == 6);
    CHECK(runs[0].front().first == Approx(150e3));
    CHECK(runs[0].back().first == Approx(300e3));
    CHECK(runs[2].front().first == Approx(5.9e6));   // SW: full band, not one point
    CHECK(runs[2].back().first == Approx(6.2e6));
    CHECK(runs[2].size() >= 2);
    CHECK(runs[3].front().first == Approx(26e6));    // CB
    CHECK(runs[3].back().first == Approx(28e6));
    CHECK(runs[5].front().first == Approx(68e6));    // 68-108, not inset to 71.7
    CHECK(runs[5].back().first == Approx(108e6));
    // a step stays a step: Class A's two segments are separate runs meeting at 500 kHz
    auto classA = Hertz::limit_polyline_runs(Hertz::cispr32_class_a_mains_qp(), 150e3, 30e6, 40);
    REQUIRE(classA.size() == 2);
    CHECK(classA[0].back().first == Approx(500e3));
    CHECK(classA[0].back().second == Approx(79.0));
    CHECK(classA[1].front().first == Approx(500e3));
    CHECK(classA[1].front().second == Approx(73.0));
}

TEST_CASE("Unswept regions name what the scan never reached (R10-1, round 10)", "[limits]") {
    auto regions = Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), 1e6, 10e6);
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].first == Approx(150e3));   // merged across the 500 kHz boundary
    CHECK(regions[0].second == Approx(1e6));
    CHECK(regions[1].first == Approx(10e6));
    CHECK(regions[1].second == Approx(30e6));
    auto c25 = Hertz::unswept_regions(
        Hertz::cispr25_conducted_voltage(3, Hertz::Detector::QUASI_PEAK), 150e3, 30e6);
    REQUIRE(c25.size() == 2);
    CHECK(c25[0].first == Approx(30e6));
    CHECK(c25[0].second == Approx(54e6));
    CHECK(c25[1].first == Approx(68e6));
    CHECK(c25[1].second == Approx(108e6));
    CHECK(Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), 150e3, 30e6).empty());
}

TEST_CASE("Unswept regions catch interior sampling holes (R11-1, round 11)", "[limits]") {
    // spliced scan: 0.15-1 MHz + 20-30 MHz — the 1-20 MHz hole must be named
    std::vector<double> freqs;
    for (int i = 0; i <= 60; ++i) freqs.push_back(150e3 * std::pow(1e6 / 150e3, i / 60.0));
    for (int i = 0; i <= 40; ++i) freqs.push_back(20e6 * std::pow(30e6 / 20e6, i / 40.0));
    auto regions = Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), freqs);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].first == Approx(1e6));
    CHECK(regions[0].second == Approx(20e6));
    // two spot samples: the whole interior is a hole
    auto spot = Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(),
                                       std::vector<double>{150e3, 30e6});
    REQUIRE(spot.size() == 1);
    CHECK(spot[0].first == Approx(150e3));
    CHECK(spot[0].second == Approx(30e6));
    // a CISPR 25 band-by-band acquisition is NOT a hole: jumps land in
    // unregulated space between the bands
    auto c25 = Hertz::cispr25_conducted_voltage(3, Hertz::Detector::QUASI_PEAK);
    std::vector<double> bands;
    for (auto [f0, f1] : {std::pair{150e3, 300e3}, {530e3, 1.8e6}, {5.9e6, 6.2e6},
                          {26e6, 28e6}, {30e6, 54e6}, {68e6, 108e6}}) {
        for (int i = 0; i <= 19; ++i) bands.push_back(f0 * std::pow(f1 / f0, i / 19.0));
    }
    CHECK(Hertz::unswept_regions(c25, bands).empty());
}

TEST_CASE("Unswept slivers below the RBW are suppressed (F12-2, round 12)", "[limits]") {
    CHECK(Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), 150e3, 30e6 - 1.0).empty());
    auto kept = Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), 150e3, 30e6 - 50e3);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].first == Approx(30e6 - 50e3));
    CHECK(kept[0].second == Approx(30e6));
    // the 6 kHz sliver at the CISPR 25 SW band edge is below the 9 kHz RBW
    auto c25 = Hertz::unswept_regions(
        Hertz::cispr25_conducted_voltage(3, Hertz::Detector::QUASI_PEAK), 150e3, 6.194e6);
    REQUIRE(c25.size() == 3);
    CHECK(c25[0].first == Approx(26e6));
}

TEST_CASE("Interior holes gated by coverage, not ratio alone (F13-1, round 13)", "[limits]") {
    // a 16.5 MHz hole at ratio 2.23 (just under the absolute rule) is still
    // grossly out of line with the scan's own spacing
    std::vector<double> freqs;
    for (int i = 0; i <= 299; ++i) freqs.push_back(150e3 * std::pow(13.45e6 / 150e3, i / 299.0));
    freqs.push_back(30e6);
    auto regions = Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), freqs);
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].first == Approx(13.45e6));
    CHECK(regions[0].second == Approx(30e6));
    // an ENTIRE skipped CISPR 25 band inside a modest 25->30 MHz jump (1.2x)
    auto c25 = Hertz::cispr25_conducted_voltage(5, Hertz::Detector::QUASI_PEAK);
    std::vector<double> covered;
    for (auto [f0, f1] : {std::pair{150e3, 300e3}, {530e3, 1.8e6}, {5.9e6, 6.2e6}}) {
        for (int i = 0; i <= 39; ++i) covered.push_back(f0 * std::pow(f1 / f0, i / 39.0));
    }
    covered.push_back(25e6);
    for (auto [f0, f1] : {std::pair{30e6, 54e6}, {68e6, 108e6}}) {
        for (int i = 0; i <= 39; ++i) covered.push_back(f0 * std::pow(f1 / f0, i / 39.0));
    }
    auto skipped = Hertz::unswept_regions(c25, covered);
    REQUIRE(skipped.size() == 1);
    CHECK(skipped[0].first == Approx(26e6));
    CHECK(skipped[0].second == Approx(28e6));
    // blessed: uniformly sparse log sweep and coarse linear sweep stay silent
    std::vector<double> sparse;
    for (int i = 0; i <= 7; ++i) sparse.push_back(150e3 * std::pow(30e6 / 150e3, i / 7.0));
    CHECK(Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), sparse).empty());
    std::vector<double> linear;
    for (double f = 150e3; f <= 30e6 + 1.0; f += 50e3) linear.push_back(f);
    CHECK(Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), linear).empty());
}

TEST_CASE("Hole detection is local, not global (F14-1, round 14)", "[limits]") {
    auto c25 = Hertz::cispr25_conducted_voltage(5, Hertz::Detector::QUASI_PEAK);
    // gutted LW band: only the 150 kHz edge sample survives; detection must
    // not depend on the sampling step 100 MHz away
    for (double upperStep : {10e3, 120e3}) {
        std::vector<double> freqs{150e3};
        for (double f = 310e3; f < 30e6; f += 10e3) freqs.push_back(f);
        for (double f = 30e6; f <= 108e6 + 1.0; f += upperStep) freqs.push_back(f);
        auto regions = Hertz::unswept_regions(c25, freqs);
        REQUIRE(!regions.empty());
        CHECK(regions[0].first == Approx(150e3));
        CHECK(regions[0].second == Approx(300e3));
    }
    // a spliced dense+coarse scan never flags its own density transition
    std::vector<double> spliced;
    for (double f = 150e3; f < 30e6; f += 1e3) spliced.push_back(f);
    for (double f = 30e6; f <= 108e6 + 1.0; f += 120e3) spliced.push_back(f);
    CHECK(Hertz::unswept_regions(c25, spliced).empty());
    // two edge samples do not measure a band
    std::vector<double> edges;
    for (auto [f0, f1] : {std::pair{150e3, 300e3}, {530e3, 1.8e6}, {5.9e6, 6.2e6}}) {
        for (int i = 0; i <= 199; ++i) edges.push_back(f0 * std::pow(f1 / f0, i / 199.0));
    }
    edges.push_back(26e6);
    edges.push_back(28e6);
    for (auto [f0, f1] : {std::pair{30e6, 54e6}, {68e6, 108e6}}) {
        for (int i = 0; i <= 199; ++i) edges.push_back(f0 * std::pow(f1 / f0, i / 199.0));
    }
    auto swallowed = Hertz::unswept_regions(c25, edges);
    REQUIRE(swallowed.size() == 1);
    CHECK(swallowed[0].first == Approx(26e6));
    CHECK(swallowed[0].second == Approx(28e6));
}

TEST_CASE("Hole width gate is frequency-independent (F15-1, round 15)", "[limits]") {
    // an identical 1.85 MHz dropout in a uniform 10 kHz sweep must be
    // reported wherever it sits — the old 0.05-decade log-ratio floor made
    // it visible at 15.1 MHz and invisible at 15.2 MHz
    for (double holeStart : {8e6, 15.2e6, 20e6, 25e6}) {
        std::vector<double> freqs;
        for (double f = 150e3; f <= 30e6 + 1.0; f += 10e3) {
            if (!(holeStart < f && f < holeStart + 1.85e6)) {
                freqs.push_back(f);
            }
        }
        auto regions = Hertz::unswept_regions(Hertz::cispr32_class_b_mains_qp(), freqs);
        REQUIRE(regions.size() == 1);
        CHECK(regions[0].first == Approx(holeStart).epsilon(1e-3));
    }
    // grid-alignment slivers stay suppressed: a 60 kHz-stepped CISPR 25 scan
    // leaves 20 kHz (2.2 RBW bins) at the top of the CB band — not a hole
    std::vector<double> grid;
    for (double f = 150e3; f <= 108e6 + 1.0; f += 60e3) grid.push_back(f);
    CHECK(Hertz::unswept_regions(
              Hertz::cispr25_conducted_voltage(5, Hertz::Detector::QUASI_PEAK), grid).empty());
}

TEST_CASE("CISPR 32 radiated limits and the CM-radiator screening model", "[radiated]") {
    auto b10 = Hertz::cispr32_radiated('B', 10.0);
    CHECK(b10.level(100e6) == Approx(30.0));
    CHECK(b10.level(500e6) == Approx(37.0));
    CHECK(b10.level(230e6) == Approx(30.0));  // lower limit at the boundary
    CHECK(Hertz::cispr32_radiated('B', 3.0).level(100e6) == Approx(40.0));
    CHECK(Hertz::cispr32_radiated('A', 10.0).level(500e6) == Approx(47.0));
    CHECK_THROWS_AS(b10.level(29e6), Hertz::OutsideCoverage);
    CHECK_THROWS_AS(Hertz::cispr32_radiated('C', 10.0), std::invalid_argument);
    CHECK_THROWS_AS(Hertz::cispr32_radiated('B', 5.0), std::invalid_argument);
    // 5 uA on 1 m at 30 MHz, 3 m -> 35.97 dBuV/m (the "5 uA fails Class B" folklore)
    auto e = Hertz::radiated_efield_dbuvm({30e6}, {20.0 * std::log10(5.0)}, 1.0, 3.0);
    CHECK(e[0] == Approx(35.97).margin(0.02));
    // lambda/4 clamp above cable resonance
    auto eLong = Hertz::radiated_efield_dbuvm({300e6}, {14.0}, 1.0, 3.0);
    auto eQuarter = Hertz::radiated_efield_dbuvm({300e6}, {14.0}, 0.25, 3.0);
    CHECK(eLong[0] == Approx(eQuarter[0]));
    CHECK_THROWS_AS(Hertz::radiated_efield_dbuvm({30e6}, {10.0}, 0.0, 3.0), std::invalid_argument);
}

TEST_CASE("dBuA current spectra pass through with their unit reported", "[traces]") {
    auto trace = Hertz::parse_spectrum_csv("Frequency (MHz),CM current (dBuA)\n30,20\n100,14\n");
    CHECK(trace.levelUnit == "dbua");
    CHECK(trace.levelsDbuv[0] == Approx(20.0));
    auto voltage = Hertz::parse_spectrum_csv("Frequency (MHz),Level (dBuV)\n1,40\n2,41\n");
    CHECK(voltage.levelUnit == "dbuv");
}

TEST_CASE("Cap ESL caps the high-frequency insertion loss (#290)", "[network]") {
    // 1 uF with 20 nH ESL self-resonates at 1.125 MHz; above it the branch is
    // inductive and IL must fall away from the ideal curve
    double ideal = Hertz::insertion_loss_db(
        Hertz::lc_filter_abcd(10e6, 14.6e-6, 1e-6, 1), {100.0, 0.0}, {100.0, 0.0});
    double real = Hertz::insertion_loss_db(
        Hertz::lc_filter_abcd(10e6, 14.6e-6, 1e-6, 1, 20e-9), {100.0, 0.0}, {100.0, 0.0});
    CHECK(real < ideal - 10.0);
    double loI = Hertz::insertion_loss_db(
        Hertz::lc_filter_abcd(150e3, 14.6e-6, 1e-6, 1), {100.0, 0.0}, {100.0, 0.0});
    double loR = Hertz::insertion_loss_db(
        Hertz::lc_filter_abcd(150e3, 14.6e-6, 1e-6, 1, 20e-9), {100.0, 0.0}, {100.0, 0.0});
    CHECK(loR == Approx(loI).margin(0.2));
    CHECK_THROWS_AS(Hertz::lc_filter_abcd(1e6, 1e-3, 1e-6, 1, -1e-9), std::invalid_argument);
}

TEST_CASE("Margin sign convention", "[limits]") {
    CHECK(Hertz::cispr32_class_b_mains_qp().margin(200e3, 50.0) > 0.0);
    CHECK(Hertz::cispr32_class_b_mains_qp().margin(200e3, 80.0) < 0.0);
}

TEST_CASE("LISN impedance", "[lisn]") {
    auto lisn50 = Hertz::cispr16_lisn();
    CHECK(std::abs(lisn50.eut_impedance(30e6)) == Approx(50.0).epsilon(0.03));
    double zBandEdge = std::abs(lisn50.eut_impedance(150e3));
    CHECK(zBandEdge > 30.0);
    CHECK(zBandEdge < 45.0);

    auto lisn5 = Hertz::cispr25_lisn();
    CHECK(std::abs(lisn5.eut_impedance(150e3)) < zBandEdge);
    CHECK(std::abs(lisn5.eut_impedance(108e6)) == Approx(50.0).epsilon(0.03));

    CHECK_THROWS_AS(lisn50.eut_impedance(0.0), std::invalid_argument);
}

TEST_CASE("LISN SPICE subcircuit", "[lisn]") {
    std::string text = Hertz::cispr16_lisn().to_spice_subckt("LISN50");
    CHECK(text.find(".subckt LISN50 eut mains meas") != std::string::npos);
    CHECK(text.find("L1 eut mains 5e-05") != std::string::npos);
    CHECK(text.find("C1 eut meas 1e-07") != std::string::npos);
    CHECK(text.find("R1 meas 0 50") != std::string::npos);
    std::string open = Hertz::cispr25_lisn().to_spice_subckt("LISN5", false);
    CHECK(open.find("R1") == std::string::npos);
}

TEST_CASE("ANP015 design frequency", "[filter]") {
    CHECK(Hertz::design_frequency(300e3) == Approx(300e3));
    CHECK(Hertz::design_frequency(100e3) == Approx(200e3));
    CHECK(Hertz::design_frequency(150e3) == Approx(150e3));
    CHECK_THROWS_AS(Hertz::design_frequency(0.0), std::invalid_argument);
}

TEST_CASE("ANP015 single-stage worked example", "[filter]") {
    auto design = Hertz::design_line_filter(
        300e3, 40.0, 40.0, 4.7e-9, Hertz::leakage_inductance_from_impedance(92.0, 1e6), 1,
        {1e-3, 1.5e-3, 2.2e-3, 3.3e-3, 4.7e-3}, {1e-6, 1.5e-6, 2.2e-6, 3.3e-6});
    CHECK(design.fCutoffCmHz == Approx(30e3));
    CHECK(design.fCutoffDmHz == Approx(30e3));
    CHECK(design.cYgF == Approx(9.4e-9));
    CHECK(design.lCmRequiredH == Approx(2.994e-3).epsilon(1e-3));
    CHECK(design.lCmSelectedH == Approx(3.3e-3));
    CHECK(design.attenuationCmDb == Approx(40.85).margin(0.05));  // note: "41 dB"
    CHECK(design.lDmH == Approx(14.64e-6).epsilon(1e-3));
    CHECK(design.cXRequiredF == Approx(1.922e-6).epsilon(1e-3));
    CHECK(design.cXSelectedF == Approx(2.2e-6));
    CHECK(design.attenuationDmDb == Approx(41.2).margin(0.1));  // note: "41 dB"
}

TEST_CASE("ANP015 two-stage worked example", "[filter]") {
    auto design = Hertz::design_line_filter(
        300e3, 40.0, 40.0, 2.2e-9, Hertz::leakage_inductance_from_impedance(41.0, 1e6), 2,
        {1e-3, 2.2e-3, 3.3e-3}, {560e-9, 1e-6});
    CHECK(design.fCutoffCmHz == Approx(94868.3).epsilon(1e-4));
    CHECK(design.lCmRequiredH == Approx(0.6396e-3).epsilon(1e-3));
    CHECK(design.lCmSelectedH == Approx(1e-3));
    CHECK(design.attenuationCmDb == Approx(47.8).margin(0.1));  // note: "48 dB"
    CHECK(design.cXRequiredF == Approx(431e-9).epsilon(1e-2));
    CHECK(design.cXSelectedF == Approx(560e-9));
    CHECK(design.attenuationDmDb == Approx(44.5).margin(0.1));  // note: "45 dB"
}

TEST_CASE("Per-mode requirements are independent (F-2, round 6)", "[filter]") {
    // CM and DM are separate networks — the quiet mode must NOT be sized from
    // the loud mode's requirement (was max(A_cm, A_dm): 224x over-design).
    auto mixed = Hertz::design_line_filter(
        300e3, 40.0, 10.0, 4.7e-9, 14.64e-6, 1,
        {0.47e-3, 1e-3, 2.2e-3, 3.3e-3}, {47e-9, 100e-9, 220e-9, 1e-6, 2.2e-6});
    CHECK(mixed.fCutoffCmHz == Approx(30e3));
    CHECK(mixed.fCutoffDmHz == Approx(300e3 / std::pow(10.0, 10.0 / 40.0)));
    CHECK(mixed.lCmSelectedH == Approx(3.3e-3));
    CHECK(mixed.cXSelectedF == Approx(100e-9));  // NOT the 2.2 uF a 40 dB DM would force
    auto mirrored = Hertz::design_line_filter(
        300e3, 10.0, 40.0, 4.7e-9, 14.64e-6, 1,
        {0.47e-3, 1e-3, 2.2e-3, 3.3e-3}, {47e-9, 100e-9, 220e-9, 1e-6, 2.2e-6});
    CHECK(mirrored.lCmSelectedH == Approx(0.47e-3));
    CHECK(mirrored.cXSelectedF == Approx(2.2e-6));
}

TEST_CASE("Per-mode design frequencies (measurement-driven callers)", "[filter]") {
    // Berger's round-6 case A: 26 dB @ 2 MHz CM -> f_co 447.7 kHz, L 13.44 uH
    auto design = Hertz::design_line_filter(
        300e3, 26.0, 33.0, 4.7e-9, 14.64e-6, 1,
        {10e-6, 47e-6, 470e-6, 3.3e-3}, {47e-9, 1e-6, 2.2e-6}, 2e6, 200e3);
    CHECK(design.fCutoffCmHz == Approx(447744.2).epsilon(1e-4));
    CHECK(design.lCmRequiredH == Approx(13.44e-6).epsilon(1e-2));
    CHECK(design.lCmSelectedH == Approx(47e-6));
    CHECK(design.fCutoffDmHz == Approx(200e3 / std::pow(10.0, 33.0 / 40.0)));
    // a 0 dB mode is legal: resonance AT the design frequency suffices
    auto silent = Hertz::design_line_filter(
        300e3, 26.0, 0.0, 4.7e-9, 14.64e-6, 1, {47e-6}, {47e-9, 1e-6}, 2e6, 2e6);
    CHECK(silent.fCutoffDmHz == Approx(2e6));
    CHECK(silent.cXSelectedF == Approx(47e-9));
}

TEST_CASE("ANP015 already-passing input throws", "[filter]") {
    CHECK_THROWS_AS(Hertz::design_line_filter(300e3, -3.0, -5.0, 4.7e-9, 14.6e-6, 1, {3.3e-3}, {2.2e-6}),
                    std::invalid_argument);
}

TEST_CASE("ANP015 leakage current and discharge resistor", "[filter]") {
    double current = Hertz::y_capacitor_leakage_current(253.0, 50.0, 5.6e-9, 5.6e-9, 2.4e-6);
    CHECK(current == Approx(0.889e-3).epsilon(0.01));
    CHECK(current < 3.5e-3);

    double rMax = Hertz::max_discharge_resistance(2.52e-6, 200.0 * std::numbers::sqrt2, 60.0, 1.0);
    CHECK(rMax == Approx(255.9e3).epsilon(0.01));
    CHECK(Hertz::round_down_to_series(rMax, Hertz::E24) == Approx(240e3));
    CHECK(Hertz::discharge_resistor_power(253.0, 240e3) == Approx(0.267).epsilon(0.01));
    CHECK_THROWS_AS(Hertz::max_discharge_resistance(2.52e-6, 60.0, 60.0, 1.0), std::invalid_argument);
}

TEST_CASE("Rounding helpers", "[utils]") {
    CHECK(Hertz::round_up_to_series(2.994e-3, Hertz::E6) == Approx(3.3e-3));
    CHECK(Hertz::round_up_to_series(1.922e-6, Hertz::E6) == Approx(2.2e-6));
    CHECK(Hertz::round_up_to(0.64e-3, {1e-3, 2.2e-3}) == Approx(1e-3));
    CHECK(Hertz::round_down_to(255.9e3, {220e3, 240e3, 270e3}) == Approx(240e3));
    CHECK_THROWS_AS(Hertz::round_up_to(5.0, {1.0, 2.0}), std::invalid_argument);
    CHECK_THROWS_AS(Hertz::round_up_to(-1.0, {1.0}), std::invalid_argument);
}

TEST_CASE("dB conversions", "[utils]") {
    CHECK(Hertz::dbuv_from_vrms(1.0) == Approx(120.0));
    CHECK(Hertz::vrms_from_dbuv(120.0) == Approx(1.0));
    CHECK(Hertz::dbuv_from_dbm(0.0) == Approx(106.99).margin(0.01));
    CHECK(std::isinf(Hertz::dbuv_from_vrms(0.0)));
}

TEST_CASE("CM/DM separation", "[separation]") {
    std::vector<double> cmTrue, dmTrue, vLine, vNeutral;
    for (int i = 0; i < 1000; ++i) {
        double t = i / 1000.0;
        cmTrue.push_back(0.3 * std::sin(2.0 * std::numbers::pi * 5.0 * t));
        dmTrue.push_back(0.1 * std::sin(2.0 * std::numbers::pi * 9.0 * t));
        vLine.push_back(cmTrue.back() + dmTrue.back());
        vNeutral.push_back(cmTrue.back() - dmTrue.back());
    }
    auto [cm, dm] = Hertz::separate(vLine, vNeutral);
    for (size_t i = 0; i < cm.size(); ++i) {
        CHECK(cm[i] == Approx(cmTrue[i]).margin(1e-12));
        CHECK(dm[i] == Approx(dmTrue[i]).margin(1e-12));
    }
    CHECK_THROWS_AS(Hertz::separate<double>({1.0}, {1.0, 2.0}), std::invalid_argument);
}

TEST_CASE("CSV parsing with units in header", "[traces]") {
    auto trace = Hertz::parse_spectrum_csv("Frequency [MHz];Level [dB\xC2\xB5V]\n0.15;62.1\n0.5;55.0\n1.0;48.3\n");
    REQUIRE(trace.frequenciesHz.size() == 3);
    CHECK(trace.frequenciesHz[0] == Approx(150e3));
    CHECK(trace.frequenciesHz[2] == Approx(1e6));
    CHECK(trace.levelsDbuv[0] == Approx(62.1));
}

TEST_CASE("CSV dBm conversion and sorting", "[traces]") {
    auto trace = Hertz::parse_spectrum_csv("Freq (Hz),Amplitude (dBm)\n2000000,-70\n1000000,-60\n");
    CHECK(trace.frequenciesHz[0] == Approx(1e6));
    CHECK(trace.levelsDbuv[0] == Approx(46.99).margin(0.01));
}

TEST_CASE("CSV stated header unit beats override (R-1 in the library)", "[traces]") {
    auto trace = Hertz::parse_spectrum_csv("Frequency,Level (dBm)\n0.2,-10\n0.5,-10\n",
                                           "MHz", "dBuV");
    CHECK(trace.levelsDbuv[0] == Approx(96.99).margin(0.01));  // dBm honored
    CHECK(trace.frequenciesHz[0] == Approx(200e3));            // override filled the gap
}

TEST_CASE("CSV ambiguous units throw, explicit units work", "[traces]") {
    std::string content = "col1,col2\n0.15,62.1\n0.5,55.0\n";
    CHECK_THROWS_AS(Hertz::parse_spectrum_csv(content), Hertz::TraceFormatError);
    auto trace = Hertz::parse_spectrum_csv(content, "MHz", "dBuV");
    CHECK(trace.frequenciesHz[0] == Approx(150e3));
}

namespace {
std::vector<double> make_tone(double durationS, double fsHz, double toneHz, double amplitude,
                              bool gated = false) {
    std::vector<double> x(static_cast<size_t>(durationS * fsHz));
    for (size_t i = 0; i < x.size(); ++i) {
        double t = static_cast<double>(i) / fsHz;
        double sample = amplitude * std::sin(2.0 * std::numbers::pi * toneHz * t);
        if (gated && std::fmod(t * 100.0, 1.0) >= 0.1) {
            sample = 0.0;  // 100 Hz PRF, 10 % duty
        }
        x[i] = sample;
    }
    return x;
}
constexpr double TONE_HZ = 300e3;
constexpr double FS_HZ = 1e6;
constexpr double AMPLITUDE = 1e-3;
const double CW_DBUV = 20.0 * std::log10(AMPLITUDE / std::numbers::sqrt2 / 1e-6);  // 56.99
}  // namespace

TEST_CASE("Multi-trace export requires a level-column choice (F-1, round 6)", "[traces]") {
    // Peak/QP/Average side by side: judging the silently-taken second column
    // turned a failing QP scan into a passing Average read.
    const std::string multi =
        "Frequency (MHz),Average (dBuV),Quasi-peak (dBuV)\n0.15,50,72\n0.30,47,69\n";
    CHECK_THROWS_WITH(Hertz::parse_spectrum_csv(multi),
                      Catch::Matchers::ContainsSubstring("multiple level columns"));
    auto columns = Hertz::spectrum_csv_columns(multi);
    CHECK(columns.count == 3);
    CHECK(columns.frequencyColumn == 1);
    REQUIRE(columns.names.size() == 3);
    CHECK(columns.names[2] == "Quasi-peak (dBuV)");
    auto qp = Hertz::parse_spectrum_csv(multi, std::nullopt, std::nullopt, 50.0, 3);
    CHECK(qp.levelsDbuv[0] == Approx(72.0));
    auto avg = Hertz::parse_spectrum_csv(multi, std::nullopt, std::nullopt, 50.0, 2);
    CHECK(avg.levelsDbuv[0] == Approx(50.0));
    CHECK_THROWS_AS(Hertz::parse_spectrum_csv(multi, std::nullopt, std::nullopt, 50.0, 4),
                    Hertz::TraceFormatError);
}

TEST_CASE("Index-first export uses the stated frequency column (R7-1, round 7)", "[traces]") {
    // "No.,Frequency (MHz),QP" — the row index must never be read as megahertz.
    const std::string idx =
        "No.,Frequency (MHz),Quasi-peak (dBuV)\n1,0.15,72\n2,0.30,69\n3,0.50,62\n";
    CHECK_THROWS_WITH(Hertz::parse_spectrum_csv(idx),
                      Catch::Matchers::ContainsSubstring("1: \"No.\""));
    auto columns = Hertz::spectrum_csv_columns(idx);
    CHECK(columns.frequencyColumn == 2);
    auto trace = Hertz::parse_spectrum_csv(idx, std::nullopt, std::nullopt, 50.0, 3);
    REQUIRE(trace.frequenciesHz.size() == 3);
    CHECK(trace.frequenciesHz[0] == Approx(150e3));  // NOT 1 MHz
    CHECK(trace.frequenciesHz[2] == Approx(500e3));
    CHECK(trace.levelsDbuv[0] == Approx(72.0));
    CHECK_THROWS_WITH(Hertz::parse_spectrum_csv(idx, std::nullopt, std::nullopt, 50.0, 2),
                      Catch::Matchers::ContainsSubstring("is the frequency column"));
    // two frequency-looking columns are ambiguous, never guessed between
    const std::string twoFreq =
        "Frequency (Hz),Start Frequency (Hz),Level (dBuV)\n150000,1,72\n500000,2,62\n";
    CHECK_THROWS_WITH(Hertz::parse_spectrum_csv(twoFreq, std::nullopt, std::nullopt, 50.0, 3),
                      Catch::Matchers::ContainsSubstring("look like the frequency axis"));
}

TEST_CASE("Decimal-comma semicolon export (F-3, round 6)", "[traces]") {
    // German-locale ';' export with ',' decimals was shredded into a 0 Hz row
    // by column-count delimiter election.
    const std::string german = "Frequenz [MHz];Pegel [dBuV]\n0,15;72,5\n0,50;62,0\n1,00;60,0\n";
    auto trace = Hertz::parse_spectrum_csv(german);
    REQUIRE(trace.frequenciesHz.size() == 3);
    CHECK(trace.frequenciesHz[0] == Approx(150e3));
    CHECK(trace.levelsDbuv[0] == Approx(72.5));
}

TEST_CASE("Non-positive or non-finite trace values throw (F-3, round 6)", "[traces]") {
    CHECK_THROWS_WITH(
        Hertz::parse_spectrum_csv("Frequency (Hz),Level (dBuV)\n0,10\n150000,72\n500000,62\n"),
        Catch::Matchers::ContainsSubstring("non-positive frequency"));
    CHECK_THROWS_WITH(
        Hertz::parse_spectrum_csv("Frequency (Hz),Level (dBuV)\n150000,NaN\n500000,62\n"),
        Catch::Matchers::ContainsSubstring("non-finite"));
}

TEST_CASE("Preamble metadata never contradicts a stated column unit (F-5, round 6)", "[traces]") {
    // "RBW 9 kHz" above a column stating Hz: the column header is the closer
    // context and decides — no false ambiguity, and no override can beat it.
    const std::string preamble = "RBW 9 kHz\nFrequency (Hz),Level (dBuV)\n150000,72\n500000,62\n";
    auto plain = Hertz::parse_spectrum_csv(preamble);
    CHECK(plain.frequenciesHz[0] == Approx(150e3));
    auto overridden = Hertz::parse_spectrum_csv(preamble, "MHz");  // must lose to the stated Hz
    CHECK(overridden.frequenciesHz[0] == Approx(150e3));
    // unlabelled columns still consult the preamble; two units there stay
    // ambiguous and need the override
    const std::string sweep = "Start 150 kHz Stop 30 MHz\ncol1,col2\n0.15,62.1\n0.5,55.0\n";
    CHECK_THROWS_AS(Hertz::parse_spectrum_csv(sweep, std::nullopt, "dBuV"), Hertz::TraceFormatError);
    auto resolved = Hertz::parse_spectrum_csv(sweep, "MHz", "dBuV");
    CHECK(resolved.frequenciesHz[0] == Approx(150e3));
}

TEST_CASE("Band lookup", "[detector]") {
    CHECK(&Hertz::band_for_frequency(TONE_HZ) == &Hertz::BAND_B);
    CHECK_THROWS_AS(Hertz::band_for_frequency(2e9), std::invalid_argument);
}

TEST_CASE("Envelope calibration on CW", "[detector]") {
    std::vector<double> freqs;
    auto envelope = Hertz::stft_envelope(make_tone(0.02, FS_HZ, TONE_HZ, AMPLITUDE), FS_HZ,
                                         Hertz::BAND_B, &freqs);
    double maxEnvelope = 0.0;
    size_t peakBin = 0;
    for (const auto& row : envelope) {
        for (size_t k = 0; k < row.size(); ++k) {
            if (row[k] > maxEnvelope) {
                maxEnvelope = row[k];
                peakBin = k;
            }
        }
    }
    CHECK(maxEnvelope == Approx(AMPLITUDE).epsilon(0.03));
    CHECK(freqs[peakBin] == Approx(TONE_HZ).margin(freqs[1] - freqs[0]));
}

TEST_CASE("CW reads its RMS on all detectors", "[detector]") {
    auto reading = Hertz::measure(make_tone(1.0, FS_HZ, TONE_HZ, AMPLITUDE), FS_HZ, Hertz::BAND_B);
    auto maxOf = [](const std::vector<double>& v) {
        double m = v[0];
        for (double value : v) m = std::max(m, value);
        return m;
    };
    CHECK(maxOf(reading.peakDbuv) == Approx(CW_DBUV).margin(0.45));
    CHECK(maxOf(reading.quasiPeakDbuv) == Approx(CW_DBUV).margin(0.45));
    CHECK(maxOf(reading.averageDbuv) == Approx(CW_DBUV).margin(0.45));
}

TEST_CASE("Pulsed signal orders detectors", "[detector]") {
    auto reading =
        Hertz::measure(make_tone(1.0, FS_HZ, TONE_HZ, AMPLITUDE, true), FS_HZ, Hertz::BAND_B);
    size_t binAtTone = 0;
    double best = 1e18;
    for (size_t k = 0; k < reading.frequenciesHz.size(); ++k) {
        double distance = std::abs(reading.frequenciesHz[k] - TONE_HZ);
        if (distance < best) {
            best = distance;
            binAtTone = k;
        }
    }
    double pk = reading.peakDbuv[binAtTone];
    double qp = reading.quasiPeakDbuv[binAtTone];
    double avg = reading.averageDbuv[binAtTone];
    CHECK(avg < qp);
    CHECK(qp < pk);
    CHECK(pk - avg == Approx(20.0).margin(1.5));  // envelope mean = duty cycle
    CHECK(pk - qp < 3.0);  // 1 ms charge vs 160 ms discharge holds QP near peak
}

TEST_CASE("Short record settles by cycling", "[detector]") {
    // 120 ms used to read the meter mid-rise (~18 dB low); the chains now
    // dwell on the cycled envelope until settled.
    auto reading = Hertz::measure(make_tone(0.12, FS_HZ, TONE_HZ, AMPLITUDE), FS_HZ, Hertz::BAND_B);
    auto maxOf = [](const std::vector<double>& v) {
        double m = v[0];
        for (double value : v) m = std::max(m, value);
        return m;
    };
    CHECK(maxOf(reading.quasiPeakDbuv) == Approx(CW_DBUV).margin(0.45));
    CHECK(maxOf(reading.averageDbuv) == Approx(CW_DBUV).margin(0.45));
}

TEST_CASE("Signal too short throws", "[detector]") {
    CHECK_THROWS_AS(Hertz::measure(make_tone(0.0001, FS_HZ, TONE_HZ, AMPLITUDE), FS_HZ, Hertz::BAND_B),
                    std::invalid_argument);
}

#include "hertz/SpiceDeck.hpp"

TEST_CASE("three-phase design: one Y per line and the delta X factor", "[filter3ph]") {
    auto design = [](int nLines) {
        return Hertz::design_line_filter(150e3, 40.0, 40.0, 4.7e-9, 30e-6, 1, {1.0}, {1.0},
                                         std::nullopt, std::nullopt, nLines);
    };
    auto d2 = design(2);
    auto d3 = design(3);
    auto d4 = design(4);
    CHECK(d3.cYgF == Catch::Approx(3.0 * 4.7e-9));
    CHECK(d4.cYgF == Catch::Approx(4.0 * 4.7e-9));
    // same CM cutoff, so the required choke scales inversely with C_YG
    CHECK(d3.lCmRequiredH == Catch::Approx(d2.lCmRequiredH * 2.0 / 3.0));
    CHECK(d4.lCmRequiredH == Catch::Approx(d2.lCmRequiredH * 2.0 / 4.0));
    // delta X: every line pair sees 1.5 C, so the part to buy is 1/1.5 of the pair value
    CHECK(d3.cXDmFactor == 1.5);
    CHECK(d3.cXRequiredF == Catch::Approx(d2.cXRequiredF / 1.5));
    // star X (4-wire) sits directly across the phase-neutral DM loop
    CHECK(d4.cXDmFactor == 1.0);
    CHECK(d4.cXRequiredF == Catch::Approx(d2.cXRequiredF));
    CHECK_THROWS(design(1));
    CHECK_THROWS(design(5));
    // the DM resonance judged with the EFFECTIVE capacitance: identical required
    // effective C -> identical achieved attenuation when candidates are ideal
    auto exact3 = Hertz::design_line_filter(150e3, 40.0, 40.0, 4.7e-9, 30e-6, 1, {1.0},
                                            {d3.cXRequiredF}, std::nullopt, std::nullopt, 3);
    auto exact2 = Hertz::design_line_filter(150e3, 40.0, 40.0, 4.7e-9, 30e-6, 1, {1.0},
                                            {d2.cXRequiredF}, std::nullopt, std::nullopt, 2);
    CHECK(exact3.attenuationDmDb == Catch::Approx(exact2.attenuationDmDb).margin(1e-9));
}

TEST_CASE("three-phase SPICE deck: all-pairs K, delta X, one LISN per line", "[filter3ph]") {
    auto deck3 = Hertz::filter_spice_deck(1, 3.3e-3, 470e-9, 4.7e-9, 30e-6,
                                          Hertz::cispr16_lisn(), "dm", 3);
    for (const char* needle :
         {"Lcm_l1_1", "Lcm_l2_1", "Lcm_l3_1", "Kcm1_12", "Kcm1_13", "Kcm1_23", "Cx1_12", "Cx1_23",
          "Cx1_31", "Cy_l1_1", "Cy_l2_1", "Cy_l3_1", "Xlisn_l1", "Xlisn_l2", "Xlisn_l3",
          "Vnoise l1_src l2_src AC 1", "vdb(meas_l3)"}) {
        INFO(needle);
        CHECK(deck3.find(needle) != std::string::npos);
    }
    // 3 windings x all pairs = exactly 3 K statements per stage
    size_t kCount = 0;
    for (size_t at = deck3.find("Kcm"); at != std::string::npos; at = deck3.find("Kcm", at + 1)) {
        ++kCount;
    }
    CHECK(kCount == 3);

    auto deck4 = Hertz::filter_spice_deck(1, 3.3e-3, 470e-9, 4.7e-9, 30e-6,
                                          Hertz::cispr16_lisn(), "dm", 4);
    for (const char* needle : {"Lcm_neut_1", "Kcm1_14", "Kcm1_34", "Cx1_1n", "Cx1_2n", "Cx1_3n",
                               "Cy_neut_1", "Xlisn_neut", "Vnoise l1_src neut_src AC 1"}) {
        INFO(needle);
        CHECK(deck4.find(needle) != std::string::npos);
    }
    CHECK(deck4.find("Cx1_12") == std::string::npos);  // star, not delta

    // the 2-line deck keeps its legacy shape
    auto deck2 = Hertz::filter_spice_deck(2, 3.3e-3, 470e-9, 4.7e-9, 30e-6,
                                          Hertz::cispr16_lisn(), "cm", 2);
    for (const char* needle : {"Lcm_line_1", "Lcm_neut_2", "Kcm1_12", "Cx1 ", "Cy_line_1",
                               ".subckt LISN", "Rsrc_line", "Rsrc_neut"}) {
        INFO(needle);
        CHECK(deck2.find(needle) != std::string::npos);
    }
    CHECK_THROWS(Hertz::filter_spice_deck(1, 3.3e-3, 470e-9, 4.7e-9, 30e-6,
                                          Hertz::cispr16_lisn(), "dm", 5));
    // K out of (0,1): leakage bigger than 2 L_cm
    CHECK_THROWS(Hertz::filter_spice_deck(1, 1e-3, 470e-9, 4.7e-9, 3e-3,
                                          Hertz::cispr16_lisn(), "dm", 3));
}

TEST_CASE("choke selection respects the leakage realizability floor", "[filterK]") {
    // an HF-only CM requirement asks for nanohenries; with 14.6 uH assumed
    // leakage the pair would be unbuildable (K out of (0,1)) — the selection
    // must rise to L > L_dm/2 and SAY the floor governed
    std::vector<double> candidates{91e-9, 1e-6, 10e-6, 100e-6, 1e-3};
    auto d = Hertz::design_line_filter(150e3, 3.0, 40.0, 33e-9, 14.6e-6, 1, candidates, {1.0},
                                       15e6, std::nullopt, 2);
    CHECK(d.lCmRequiredH < 1e-6);                    // CM alone wanted almost nothing
    CHECK(d.lCmSelectedH == 10e-6);                  // smallest candidate above L_dm/2
    CHECK(d.lCmFloorFromLeakage);
    double k = 1.0 - d.lDmH / (2.0 * d.lCmSelectedH);
    CHECK(k > 0.0); CHECK(k < 1.0);
    // the deck accepts what the design selected
    CHECK_NOTHROW(Hertz::filter_spice_deck(1, d.lCmSelectedH, 470e-9, 4.7e-9, d.lDmH,
                                           Hertz::cispr16_lisn(), "dm", 2));
    // an ordinary mains design is untouched by the floor
    auto normal = Hertz::design_line_filter(150e3, 40.0, 40.0, 4.7e-9, 14.6e-6, 1, {20e-3}, {1.0});
    CHECK_FALSE(normal.lCmFloorFromLeakage);
    // no candidate above the floor: the error must name the leakage constraint
    try {
        Hertz::design_line_filter(150e3, 3.0, 40.0, 33e-9, 14.6e-6, 1, {91e-9, 1e-6}, {1.0},
                                  15e6, std::nullopt, 2);
        FAIL("expected a leakage-floor error");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("leakage") != std::string::npos);
    }
}

TEST_CASE("reference bench deck: same drive and LISNs, no filter (#299)", "[roundtrip]") {
    auto ref = Hertz::lisn_reference_deck(Hertz::cispr16_lisn(), "cm", 2);
    CHECK(ref.find("REFERENCE bench") != std::string::npos);
    CHECK(ref.find(".subckt LISN") != std::string::npos);
    CHECK(ref.find("Xlisn_line line_src") != std::string::npos);   // LISN straight on the source
    CHECK(ref.find("Lcm_") == std::string::npos);                  // no filter parts
    CHECK(ref.find("Cx") == std::string::npos);
    CHECK(ref.find("Cy_") == std::string::npos);
    CHECK(ref.find("Vnoise cm_src 0 AC 1") != std::string::npos);
    auto ref3 = Hertz::lisn_reference_deck(Hertz::cispr16_lisn(), "dm", 3);
    CHECK(ref3.find("Xlisn_l3 l3_src") != std::string::npos);
    CHECK(ref3.find("Vnoise l1_src l2_src AC 1") != std::string::npos);
}

TEST_CASE("deck-terminated ABCD twin behaves at the physical limits (#299)", "[roundtrip]") {
    std::vector<double> freqs{150e3, 1e6, 10e6, 30e6};
    auto il = Hertz::deck_abcd_il(Hertz::cispr16_lisn(), "cm", 2, 1, 1e-3, 4.7e-9, 14.6e-6,
                                  470e-9, 1.0, freqs);
    REQUIRE(il.size() == freqs.size());
    // above the CM corner the IL must grow monotonically with frequency
    CHECK(il[0] < il[1]); CHECK(il[1] < il[2]); CHECK(il[2] < il[3]);
    CHECK(il[3] > 60.0);
    // tightly-coupled stack: the CM effective inductance approaches L, so the
    // K-aware twin and a plain-L chain agree closely
    double k = 1.0 - 14.6e-6 / (2.0 * 1e-3);
    double lEff = 1e-3 * (1.0 + k) / 2.0;
    CHECK(lEff == Catch::Approx(1e-3).epsilon(0.01));
    // loose coupling (leakage-floored design): the effective CM inductance is
    // FAR from L — exactly what a plain-L cross-check would get wrong
    double kLoose = 1.0 - 14.6e-6 / (2.0 * 7.5e-6);
    double lEffLoose = 7.5e-6 * (1.0 + kLoose) / 2.0;
    CHECK(lEffLoose < 0.55 * 7.5e-6);
    CHECK_THROWS(Hertz::deck_abcd_il(Hertz::cispr16_lisn(), "cm", 2, 1, 1e-6, 4.7e-9, 14.6e-6,
                                     470e-9, 1.0, freqs));   // K out of (0,1)
}

TEST_CASE("radiated CM-attenuation target and governing point", "[radiated]") {
    const double dbua5uA = 20.0 * std::log10(5.0);  // 5 uA -> 13.979 dBuA

    // over-limit + margin, in the CM-current domain. 50 MHz E = 40.40 dBuV/m over
    // a 40 dBuV/m limit, +6 -> 6.40 dB; a quiet 1e-7 A point sits under the limit.
    auto target = Hertz::radiated_cm_attenuation_target_db(
        {50e6, 200e6}, {dbua5uA, -20.0}, 1.0, 3.0, {40.0, 40.0}, 6.0);
    CHECK(target[0] == Catch::Approx(6.40).margin(0.05));
    CHECK(target[1] < 0.0);
    CHECK_THROWS(Hertz::radiated_cm_attenuation_target_db(
        {50e6, 80e6}, {10.0, 10.0}, 1.0, 3.0, {40.0}, 6.0));  // limit length mismatch

    auto g = Hertz::governing_requirement({30e6, 80e6, 200e6}, {2.0, 8.0, 3.0});
    CHECK(g.attenuationDb == Catch::Approx(8.0));
    CHECK(g.frequencyHz == Catch::Approx(80e6));
    CHECK_THROWS(Hertz::governing_requirement({30e6, 80e6}, {-1.0, -5.0}));  // already compliant
}

TEST_CASE("cable ferrite insertion loss and candidate selection", "[cable]") {
    const Hertz::FerritePart bead150{"bead-150", {10e6, 1e9}, {150.0, 150.0}};
    const Hertz::FerritePart bead100{"bead-100", {10e6, 1e9}, {100.0, 100.0}};
    const Hertz::FerritePart bead250{"bead-250", {10e6, 1e9}, {250.0, 250.0}};

    // 150 ohm between 25 ohm refs: 20*log10((25+150+25)/50) = 20*log10(4) = 12.041 dB.
    CHECK(Hertz::ferrite_insertion_loss_db(bead150, 1, 25.0, {100e6})[0] == Catch::Approx(12.041).margin(0.01));
    // 2 turns -> 4*150 = 600 ohm: 20*log10(13) = 22.279 dB.
    CHECK(Hertz::ferrite_insertion_loss_db(bead150, 2, 25.0, {100e6})[0] == Catch::Approx(22.279).margin(0.01));
    CHECK_THROWS(Hertz::ferrite_insertion_loss_db(bead150, 1, 25.0, {5e6}));   // below the curve
    CHECK_THROWS(Hertz::ferrite_insertion_loss_db(bead150, 1, 25.0, {2e9}));   // above the curve

    // bead-100 gives 9.54 dB (misses the 10 dB point); bead-250 gives 15.56 dB.
    auto pick = Hertz::select_cable_mitigation({30e6, 100e6, 300e6}, {5.0, 10.0, 4.0},
                                               {bead100, bead250}, 25.0, 1);
    CHECK(pick.partName == "bead-250");
    CHECK(pick.turns == 1);
    CHECK(pick.meetsTarget);
    CHECK(pick.worstMarginDb == Catch::Approx(5.563).margin(0.01));

    // only bead-100: 1 turn misses, 2 turns (400 ohm -> 19.08 dB) meets.
    auto pick2 = Hertz::select_cable_mitigation({30e6, 100e6, 300e6}, {5.0, 10.0, 4.0},
                                                {bead100}, 25.0, 2);
    CHECK(pick2.partName == "bead-100");
    CHECK(pick2.turns == 2);
    CHECK(pick2.meetsTarget);

    // unreachable: best near-miss reported, never a silent pass.
    auto miss = Hertz::select_cable_mitigation({100e6}, {30.0}, {bead100}, 25.0, 1);
    CHECK_FALSE(miss.meetsTarget);
    CHECK(miss.worstMarginDb == Catch::Approx(9.542 - 30.0).margin(0.01));

    CHECK_THROWS(Hertz::select_cable_mitigation({100e6}, {-3.0}, {bead100}, 25.0));  // nothing required
    CHECK_THROWS(Hertz::select_cable_mitigation({100e6}, {5.0}, {}, 25.0));          // empty list
}
