// Comb detection: shared golden vectors with tests/test_comb.py (the synthetic
// 300 kHz comb on a linear grid, the smooth floor, the lone peak).
// Network: insertion loss validated against an independent direct node
// analysis of the same LC circuit — a different derivation than the ABCD path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

#include "hertz/CombDetection.hpp"
#include "hertz/FilterDesign.hpp"
#include "hertz/Network.hpp"

using Catch::Approx;

namespace {

void synthetic_comb(std::vector<double>& freqs, std::vector<double>& levels,
                    double fSw = 300e3) {
    const int n = 4000;
    freqs.resize(n);
    levels.resize(n);
    const double sigma = 9e3;
    for (int i = 0; i < n; ++i) {
        double f = 150e3 + (30e6 - 150e3) * i / (n - 1.0);
        double level = 28.0 - 6.0 * std::log10(f / 150e3);
        for (int h = 1; h <= 60; ++h) {
            double fh = h * fSw;
            double peak = 84.0 - 22.0 * std::log10(static_cast<double>(h));
            level = std::max(level, peak - 0.5 * std::pow((f - fh) / sigma, 2));
        }
        freqs[i] = f;
        levels[i] = level;
    }
}

void smooth_floor(std::vector<double>& freqs, std::vector<double>& levels) {
    const int n = 300;
    freqs.resize(n);
    levels.resize(n);
    for (int i = 0; i < n; ++i) {
        double f = std::pow(10.0, std::log10(150e3) +
                                      (std::log10(30e6) - std::log10(150e3)) * i / (n - 1.0));
        freqs[i] = f;
        levels[i] = 40.0 - 8.0 * std::log10(f / 150e3);
    }
}

}  // namespace

TEST_CASE("Comb: detects 300 kHz comb", "[comb]") {
    std::vector<double> freqs, levels;
    synthetic_comb(freqs, levels);
    auto result = Hertz::detect_comb(freqs, levels);
    REQUIRE(result.found);
    CHECK(result.fSwHz == Approx(300e3).epsilon(0.01));
    CHECK(result.confidence > 0.6);
    CHECK(result.harmonics.size() >= 10);
    CHECK(result.harmonics.front().order == 1);
    CHECK(result.fSwHz > 200e3);  // subharmonic must not win
}

TEST_CASE("Comb: smooth floor and lone peak find nothing", "[comb]") {
    std::vector<double> freqs, levels;
    smooth_floor(freqs, levels);
    CHECK_FALSE(Hertz::detect_comb(freqs, levels).found);
    for (size_t i = 0; i < freqs.size(); ++i) {
        levels[i] += 20.0 * std::exp(-0.5 * std::pow((freqs[i] - 12e6) / 0.2e6, 2));
    }
    CHECK_FALSE(Hertz::detect_comb(freqs, levels).found);
}

TEST_CASE("Comb: survives missing harmonics", "[comb]") {
    std::vector<double> freqs, levels;
    synthetic_comb(freqs, levels);
    for (size_t i = 0; i < freqs.size(); ++i) {
        for (int h : {3, 4}) {
            if (std::abs(freqs[i] - h * 300e3) < 30e3) {
                levels[i] = 28.0 - 6.0 * std::log10(freqs[i] / 150e3);
            }
        }
    }
    auto result = Hertz::detect_comb(freqs, levels);
    REQUIRE(result.found);
    CHECK(result.fSwHz == Approx(300e3).epsilon(0.01));
}

TEST_CASE("Comb: input validation", "[comb]") {
    CHECK_THROWS_AS(Hertz::detect_comb({1.0, 2.0}, {0.0, 0.0}), std::invalid_argument);
}

TEST_CASE("Network: ABCD insertion loss matches direct node analysis", "[network]") {
    // Single LC stage (14.64 µH / 2.2 µF, the ANP015 DM values) between
    // arbitrary terminations, solved directly: V_node = Vs·Zp/(Zs + jwL + Zp)
    // with Zp = (1/jwC) ∥ Zl; unfiltered V0 = Vs·Zl/(Zs + Zl).
    const double l = 14.64e-6, c = 2.2e-6;
    for (double f : {10e3, 100e3, 300e3, 1e6, 10e6}) {
        for (double zs : {0.1, 50.0, 100.0}) {
            for (double zl : {0.1, 50.0, 100.0}) {
                double w = 2.0 * std::numbers::pi * f;
                std::complex<double> zL(0.0, w * l);
                std::complex<double> zC(0.0, -1.0 / (w * c));
                std::complex<double> zp = zC * zl / (zC + zl);
                double direct = 20.0 * std::log10(
                    std::abs((zl / (zs + zl)) / (zp / (zs + zL + zp))));
                auto network = Hertz::lc_filter_abcd(f, l, c, 1);
                CHECK(Hertz::insertion_loss_db(network, {zs, 0.0}, {zl, 0.0}) ==
                      Approx(direct).margin(1e-9));
            }
        }
    }
}

TEST_CASE("Network: slope and worst case behave physically", "[network]") {
    const double l = 14.64e-6, c = 2.2e-6;
    double f0 = Hertz::resonant_cutoff(l, c);
    auto at = [&](double f) {
        return Hertz::insertion_loss_db(Hertz::lc_filter_abcd(f, l, c, 1), {100.0, 0.0},
                                        {100.0, 0.0});
    };
    // 40 dB/decade asymptote — above BOTH corners (1/2piRC and R/2piL), not
    // just above resonance: between them the slope is 20 dB/dec because only
    // the capacitor works against the 100 Ohm terminations.
    CHECK(at(30e6) - at(3e6) == Approx(40.0).margin(1.0));
    auto at2 = [&](double f) {
        return Hertz::insertion_loss_db(Hertz::lc_filter_abcd(f, l, c, 2), {100.0, 0.0},
                                        {100.0, 0.0});
    };
    CHECK(at2(30e6) - at2(3e6) == Approx(80.0).margin(1.5));
    // CISPR 17 worst case never beats the symmetric reference far above cutoff
    for (double f : {3.0 * f0, 10.0 * f0, 30.0 * f0}) {
        auto network = Hertz::lc_filter_abcd(f, l, c, 1);
        CHECK(Hertz::insertion_loss_worst_case_db(network) <=
              Hertz::insertion_loss_db(network, {100.0, 0.0}, {100.0, 0.0}) + 1e-9);
    }
}

TEST_CASE("Network: curves are consistent with point evaluation", "[network]") {
    auto curves = Hertz::insertion_loss_curves(14.64e-6, 2.2e-6, 1, 100.0, 150e3, 30e6, 20);
    REQUIRE(curves.frequenciesHz.size() == curves.standardDb.size());
    REQUIRE(curves.frequenciesHz.size() == curves.worstCaseDb.size());
    size_t mid = curves.frequenciesHz.size() / 2;
    auto network = Hertz::lc_filter_abcd(curves.frequenciesHz[mid], 14.64e-6, 2.2e-6, 1);
    CHECK(curves.standardDb[mid] ==
          Approx(Hertz::insertion_loss_db(network, {100.0, 0.0}, {100.0, 0.0})).margin(1e-9));
}

TEST_CASE("Network: tabulated curve of an ideal inductor matches ideal LC exactly", "[network]") {
    const double l = 14.64e-6, c = 2.2e-6;
    std::vector<double> freqs, zRe, zIm;
    for (int i = 0; i < 40; ++i) {
        double f = std::pow(10.0, 5.2 + 2.2 * i / 39.0);
        freqs.push_back(f);
        zRe.push_back(0.0);
        zIm.push_back(2.0 * std::numbers::pi * f * l);
    }
    auto curves = Hertz::tabulated_series_il_curves(freqs, zRe, zIm, c, 2, 25.0);
    for (size_t i = 0; i < freqs.size(); ++i) {
        auto network = Hertz::lc_filter_abcd(freqs[i], l, c, 2);
        CHECK(curves.standardDb[i] ==
              Approx(Hertz::insertion_loss_db(network, {25.0, 0.0}, {25.0, 0.0})).margin(1e-9));
        CHECK(curves.worstCaseDb[i] ==
              Approx(Hertz::insertion_loss_worst_case_db(network)).margin(1e-9));
    }
    CHECK_THROWS_AS(Hertz::tabulated_series_il_curves({2e6, 1e6}, {0, 0}, {1, 1}, c, 1, 25.0),
                    std::invalid_argument);
}

TEST_CASE("Middlebrook: interaction check", "[middlebrook]") {
    // ANP015 single-stage DM values feeding a 25 W flyback from 230 V mains:
    // R0 = sqrt(14.64u/2.2u) = 2.58 Ohm, Zin = 207²/25 = 1714 Ohm -> huge margin.
    auto interaction = Hertz::input_filter_interaction(14.64e-6, 2.2e-6, 207.0, 25.0);
    CHECK(interaction.characteristicImpedanceOhm == Approx(2.579).epsilon(1e-3));
    CHECK(interaction.converterInputImpedanceOhm == Approx(1713.96).epsilon(1e-3));
    CHECK(interaction.marginDb == Approx(56.45).margin(0.05));
    CHECK(interaction.dampingResistorOhm == Approx(2.579).epsilon(1e-3));
    CHECK(interaction.dampingCapacitorMinF == Approx(11e-6).epsilon(1e-6));
    CHECK(interaction.dampingCapacitorMaxF == Approx(22e-6).epsilon(1e-6));
    // A 48 V / 500 W DC-DC with the same filter is a real interaction risk:
    auto tight = Hertz::input_filter_interaction(14.64e-6, 2.2e-6, 36.0, 500.0);
    CHECK(tight.marginDb == Approx(0.05).margin(0.06));
    CHECK_THROWS_AS(Hertz::input_filter_interaction(0.0, 2.2e-6, 48.0, 100.0),
                    std::invalid_argument);
}

TEST_CASE("Network: a sub-step-narrow span yields finite points, never NaN", "[network]") {
    // Regression: a frequency span narrower than one 1/pointsPerDecade step used to
    // truncate steps to 0, so the sampling loop evaluated i/steps = 0/0 = NaN and
    // emitted a silent NaN point instead of throwing. 1.0-1.02 MHz at 40 pts/decade
    // is ~0.34 of a step (well under 1). Guard makes steps>=1 -> two finite points.
    auto curves = Hertz::insertion_loss_curves(14.64e-6, 2.2e-6, 1, 100.0, 1.0e6, 1.02e6, 40);
    REQUIRE(curves.frequenciesHz.size() >= 2);
    REQUIRE(curves.frequenciesHz.size() == curves.standardDb.size());
    for (double f : curves.frequenciesHz) {
        CHECK(std::isfinite(f));
    }
    for (double d : curves.standardDb) {
        CHECK(std::isfinite(d));
    }
    for (double d : curves.worstCaseDb) {
        CHECK(std::isfinite(d));
    }
    // endpoints must still bracket the requested span
    CHECK(curves.frequenciesHz.front() == Approx(1.0e6));
    CHECK(curves.frequenciesHz.back() == Approx(1.02e6));
}
