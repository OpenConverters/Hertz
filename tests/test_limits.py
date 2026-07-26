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
