import numpy as np
import pytest

from hertz.limits import (
    CISPR32_CLASS_A_MAINS_AVG,
    CISPR32_CLASS_A_MAINS_QP,
    CISPR32_CLASS_B_MAINS_AVG,
    CISPR32_CLASS_B_MAINS_QP,
    LimitLine,
    LimitSegment,
    OutsideCoverage,
    cispr25_conducted_voltage,
    unswept_regions,
    unswept_regions_sampled,
)


def test_cispr32_class_b_qp_breakpoints():
    line = CISPR32_CLASS_B_MAINS_QP
    assert line.level(150e3) == pytest.approx(66.0)
    assert line.level(500e3) == pytest.approx(56.0)
    assert line.level(4.9e6) == pytest.approx(56.0)
    assert line.level(5.1e6) == pytest.approx(60.0)
    assert line.level(30e6) == pytest.approx(60.0)


def test_cispr32_log_interpolation():
    # Geometric midpoint of 150-500 kHz slope sits at the arithmetic dB midpoint.
    f_mid = np.sqrt(150e3 * 500e3)
    assert CISPR32_CLASS_B_MAINS_QP.level(f_mid) == pytest.approx(61.0, abs=1e-9)
    assert CISPR32_CLASS_B_MAINS_AVG.level(f_mid) == pytest.approx(51.0, abs=1e-9)


def test_cispr32_class_a_flat_segments():
    assert CISPR32_CLASS_A_MAINS_QP.level(200e3) == pytest.approx(79.0)
    assert CISPR32_CLASS_A_MAINS_QP.level(10e6) == pytest.approx(73.0)


def test_outside_coverage_raises():
    with pytest.raises(OutsideCoverage):
        CISPR32_CLASS_B_MAINS_QP.level(100e3)
    with pytest.raises(OutsideCoverage):
        CISPR32_CLASS_B_MAINS_QP.level(50e6)


def test_cispr25_class5_lw_band():
    qp = cispr25_conducted_voltage(5, "quasi_peak")
    assert qp.level(200e3) == pytest.approx(57.0)
    assert cispr25_conducted_voltage(5, "peak").level(200e3) == pytest.approx(70.0)
    assert cispr25_conducted_voltage(5, "average").level(200e3) == pytest.approx(50.0)


def test_cispr25_class_steps_are_band_dependent():
    # Table 4 (F8-2, round 8): 10 dB LW, 8 dB MW, 6 dB from SW up. The old
    # flat +10/+20 derivation was up to 8 dB too permissive.
    qp = lambda cls, f: cispr25_conducted_voltage(cls, "quasi_peak").level(f)
    assert qp(4, 200e3) == pytest.approx(67.0)   # LW: 57 + 10
    assert qp(3, 200e3) == pytest.approx(77.0)
    assert qp(4, 1e6) == pytest.approx(49.0)     # MW: 41 + 8
    assert qp(3, 1e6) == pytest.approx(57.0)
    assert qp(4, 6e6) == pytest.approx(46.0)     # SW: 40 + 6
    assert qp(3, 6e6) == pytest.approx(52.0)
    assert qp(3, 27e6) == pytest.approx(43.0)    # CB: 31 + 12
    assert qp(3, 90e6) == pytest.approx(37.0)    # FM: 25 + 12


def test_cispr25_vhf_bands_exist():
    # F8-4 (round 8): 30-54 and 68-87 MHz voltage-method bands are in Table 4
    qp = cispr25_conducted_voltage(5, "quasi_peak")
    assert qp.level(40e6) == pytest.approx(31.0)
    assert qp.level(70e6) == pytest.approx(25.0)
    assert qp.level(80e6) == pytest.approx(25.0)  # 68-87 mobile == 76-108 FM values
    with pytest.raises(OutsideCoverage):
        qp.level(60e6)   # 54-68 MHz is a real gap
    with pytest.raises(OutsideCoverage):
        qp.level(29e6)   # 28-30 MHz is a real gap


def test_unswept_regions_name_what_the_scan_never_reached():
    # R10-1 (round 10): a verdict naming a standard implies the standard's
    # whole range — a 1-10 MHz sweep against Class B leaves 150 kHz-1 MHz and
    # 10-30 MHz unmeasured (merged across the 500 kHz segment boundary).
    regions = unswept_regions(CISPR32_CLASS_B_MAINS_QP, 1e6, 10e6)
    assert regions == [(150e3, 1e6), (10e6, 30e6)]
    # a 150 kHz-30 MHz mains scan against CISPR 25 never reaches VHF/FM
    regions = unswept_regions(cispr25_conducted_voltage(3, "quasi_peak"), 150e3, 30e6)
    assert regions == [(30e6, 54e6), (68e6, 108e6)]
    # a full-span sweep leaves nothing
    assert unswept_regions(CISPR32_CLASS_B_MAINS_QP, 150e3, 30e6) == []


def test_unswept_regions_catch_interior_sampling_holes():
    # R11-1 (round 11): a spliced scan (0.15-1 MHz + 20-30 MHz, nothing in
    # between) must report the 1-20 MHz hole — the extent check alone saw a
    # full sweep. Holes are clipped to the limit's segments, so the
    # unregulated space between CISPR 25 bands is never flagged.
    freqs = list(np.geomspace(150e3, 1e6, 61)) + list(np.geomspace(20e6, 30e6, 41))
    regions = unswept_regions_sampled(CISPR32_CLASS_B_MAINS_QP, freqs)
    assert len(regions) == 1
    assert regions[0] == pytest.approx((1e6, 20e6))
    # two spot samples: the whole interior is a hole
    regions = unswept_regions_sampled(CISPR32_CLASS_B_MAINS_QP, [150e3, 30e6])
    assert regions[0] == pytest.approx((150e3, 30e6))
    # a CISPR 25 band-by-band acquisition is NOT a hole: the jumps land in
    # unregulated space
    c25 = cispr25_conducted_voltage(3, "quasi_peak")
    band_by_band = (list(np.geomspace(150e3, 300e3, 20)) +
                    list(np.geomspace(530e3, 1.8e6, 20)) +
                    list(np.geomspace(5.9e6, 6.2e6, 20)) +
                    list(np.geomspace(26e6, 28e6, 20)) +
                    list(np.geomspace(30e6, 54e6, 20)) +
                    list(np.geomspace(68e6, 108e6, 20)))
    assert unswept_regions_sampled(c25, band_by_band) == []
    # a dense uniform sweep flags nothing
    assert unswept_regions_sampled(
        CISPR32_CLASS_B_MAINS_QP, list(np.geomspace(150e3, 30e6, 100))) == []


def test_unswept_slivers_below_rbw_are_suppressed():
    # F12-2 (round 12): a sweep ending 1 Hz short of 30 MHz (or float dust
    # from building the frequency list) must not print "NOT SWEPT:
    # 30 MHz-30 MHz" — a region narrower than the receiver's RBW cannot even
    # be resolved as unmeasured. A real 50 kHz shortfall stays reported.
    freqs = list(np.geomspace(150e3, 30e6 - 1.0, 401))
    assert unswept_regions_sampled(CISPR32_CLASS_B_MAINS_QP, freqs) == []
    assert unswept_regions(CISPR32_CLASS_B_MAINS_QP, 150e3, 30e6 - 1.0) == []
    kept = unswept_regions(CISPR32_CLASS_B_MAINS_QP, 150e3, 30e6 - 50e3)
    assert len(kept) == 1
    assert kept[0] == pytest.approx((30e6 - 50e3, 30e6))
    # the 6 kHz band-edge sliver of a 7 kHz-stepped CISPR 25 SW sweep is
    # below the 9 kHz RBW: suppressed
    c25 = cispr25_conducted_voltage(3, "quasi_peak")
    assert unswept_regions(c25, 150e3, 6.194e6) == [(26e6, 28e6), (30e6, 54e6), (68e6, 108e6)]


def test_interior_holes_are_gated_by_coverage_not_ratio_alone():
    # F13-1 (round 13): a 16.5 MHz hole at a frequency ratio of 2.23 (just
    # under the 2.24 absolute rule) must still be flagged — it is grossly out
    # of line with the scan's own spacing.
    freqs = list(np.geomspace(150e3, 13.45e6, 300)) + [30e6]
    regions = unswept_regions_sampled(CISPR32_CLASS_B_MAINS_QP, freqs)
    assert len(regions) == 1
    assert regions[0] == pytest.approx((13.45e6, 30e6))
    # an ENTIRE skipped CISPR 25 band inside a modest 25->30 MHz jump (1.2x)
    c25 = cispr25_conducted_voltage(5, "quasi_peak")
    covered = (list(np.geomspace(150e3, 300e3, 40)) + list(np.geomspace(530e3, 1.8e6, 40)) +
               list(np.geomspace(5.9e6, 6.2e6, 40)) + [25e6] +
               list(np.geomspace(30e6, 54e6, 40)) + list(np.geomspace(68e6, 108e6, 40)))
    regions = unswept_regions_sampled(c25, covered)
    assert regions == [pytest.approx((26e6, 28e6))]
    # blessed cases stay silent: a uniformly sparse log sweep (8 points,
    # 0.33 dec spacing) is density, not a hole...
    assert unswept_regions_sampled(
        CISPR32_CLASS_B_MAINS_QP, list(np.geomspace(150e3, 30e6, 8))) == []
    # ...and so is a coarse LINEAR-frequency export (wide log-gaps at the
    # bottom, tiny at the top)
    assert unswept_regions_sampled(
        CISPR32_CLASS_B_MAINS_QP, list(np.arange(150e3, 30e6 + 1, 50e3))) == []


def test_hole_detection_is_local_not_global():
    # F14-1 (round 14): the SAME gutted LW band (only the 150 kHz edge sample
    # survives; next sample 310 kHz) must be reported no matter how coarse the
    # sampling is 100 MHz away — Berger's A/B acceptance pair.
    c25 = cispr25_conducted_voltage(5, "quasi_peak")
    below_30 = [150e3] + list(np.arange(310e3, 30e6, 10e3))
    for upper_step in (10e3, 120e3):
        freqs = below_30 + list(np.arange(30e6, 108e6 + 1, upper_step))
        regions = unswept_regions_sampled(c25, freqs)
        assert regions[0] == pytest.approx((150e3, 300e3)), f"step {upper_step}"
    # a spliced dense+coarse scan never flags its own density transition
    spliced = list(np.arange(150e3, 30e6, 1e3)) + list(np.arange(30e6, 108e6 + 1, 120e3))
    assert unswept_regions_sampled(c25, spliced) == []
    # two edge samples do not measure a band: CB with only 26 and 28 MHz
    # sampled is swallowed regardless of any density argument
    dense_elsewhere = (list(np.geomspace(150e3, 300e3, 200)) +
                       list(np.geomspace(530e3, 1.8e6, 200)) +
                       list(np.geomspace(5.9e6, 6.2e6, 200)) + [26e6, 28e6] +
                       list(np.geomspace(30e6, 54e6, 200)) +
                       list(np.geomspace(68e6, 108e6, 200)))
    assert unswept_regions_sampled(c25, dense_elsewhere) == [pytest.approx((26e6, 28e6))]


def test_hole_width_gate_is_frequency_independent():
    # F15-1 (round 15): an identical 1.85 MHz / 185-point dropout in a uniform
    # 10 kHz sweep must be reported wherever it sits — the old 0.05-decade
    # log-ratio floor made it visible at 15.1 MHz and invisible at 15.2 MHz.
    def sweep_with_hole(hole_start):
        freqs = np.arange(150e3, 30e6 + 1, 10e3)
        return [f for f in freqs if not hole_start < f < hole_start + 1.85e6]

    for hole_start in (8e6, 15.2e6, 20e6, 25e6):
        regions = unswept_regions_sampled(CISPR32_CLASS_B_MAINS_QP, sweep_with_hole(hole_start))
        assert len(regions) == 1, f"hole at {hole_start}"
        assert regions[0][0] == pytest.approx(hole_start, rel=1e-3)
    # grid-alignment slivers stay suppressed: a 60 kHz-stepped CISPR 25 scan
    # leaves 20 kHz (2.2 RBW bins) at the top of the CB band — not a hole
    c25 = cispr25_conducted_voltage(5, "quasi_peak")
    assert unswept_regions_sampled(c25, list(np.arange(150e3, 108e6 + 1, 60e3))) == []


def test_lower_limit_applies_at_segment_boundary():
    # F9-1 (round 9), CISPR transition rule: at exactly 500 kHz Class A is 73
    # dBuV, not 79; the vectorized path must agree with the scalar one.
    assert CISPR32_CLASS_A_MAINS_QP.level(500e3) == pytest.approx(73.0)
    assert CISPR32_CLASS_A_MAINS_AVG.level(500e3) == pytest.approx(60.0)
    assert CISPR32_CLASS_B_MAINS_QP.level(500e3) == pytest.approx(56.0)
    assert CISPR32_CLASS_B_MAINS_QP.level(5e6) == pytest.approx(56.0)
    mask, levels = CISPR32_CLASS_A_MAINS_QP.levels_where_covered([400e3, 500e3, 600e3])
    assert mask.all()
    np.testing.assert_allclose(levels, [79.0, 73.0, 73.0])


def test_cispr25_gap_between_bands_raises():
    with pytest.raises(OutsideCoverage):
        cispr25_conducted_voltage(5, "quasi_peak").level(400e3)  # between LW and MW


def test_levels_where_covered():
    freqs = np.array([100e3, 200e3, 400e3, 1e6])
    mask, levels = cispr25_conducted_voltage(5, "quasi_peak").levels_where_covered(freqs)
    assert mask.tolist() == [False, True, False, True]
    assert np.isnan(levels[0]) and np.isnan(levels[2])
    assert levels[1] == pytest.approx(57.0)


def test_margin_sign_convention():
    assert CISPR32_CLASS_B_MAINS_QP.margin(200e3, 50.0) > 0.0
    assert CISPR32_CLASS_B_MAINS_QP.margin(200e3, 80.0) < 0.0


def test_overlapping_segments_rejected():
    with pytest.raises(ValueError):
        LimitLine("bad", "quasi_peak", [
            LimitSegment(150e3, 500e3, 66.0, 56.0),
            LimitSegment(400e3, 5e6, 56.0, 56.0),
        ])
