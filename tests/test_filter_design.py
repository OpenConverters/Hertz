"""Regression against the ANP015 worked example: 25 W flyback, f_sw = 300 kHz,
target 40 dB CM and DM. Expected values are the app note's own numbers."""

import math

import pytest

from hertz.filter_design import (
    design_frequency,
    design_line_filter,
    discharge_resistor_power,
    leakage_inductance_from_impedance,
    max_discharge_resistance,
    required_attenuation_db,
    y_capacitor_leakage_current,
)
from hertz.utils import E24, round_down_to_series


def test_design_frequency_above_band_start():
    assert design_frequency(300e3) == 300e3


def test_design_frequency_uses_first_harmonic_in_band():
    assert design_frequency(100e3) == 200e3
    assert design_frequency(150e3) == 150e3


def test_design_frequency_rejects_nonpositive():
    with pytest.raises(ValueError):
        design_frequency(0.0)


def test_required_attenuation():
    # ANP015 §1: 84 dBµV measured, 60 dBµV limit, 10 dB buffer -> 34 dB.
    assert required_attenuation_db(84.0, 60.0) == pytest.approx(34.0)


def test_single_stage_matches_anp015():
    design = design_line_filter(
        f_sw_hz=300e3,
        a_req_cm_db=40.0,
        a_req_dm_db=40.0,
        c_y_per_line_f=4.7e-9,
        l_dm_h=leakage_inductance_from_impedance(92.0, 1e6),  # 14.6 µH from Z@1 MHz
        stages=1,
        l_cm_candidates=[1e-3, 1.5e-3, 2.2e-3, 3.3e-3, 4.7e-3],
        c_x_candidates=[1e-6, 1.5e-6, 2.2e-6, 3.3e-6],
    )
    assert design.f_cutoff_target_hz == pytest.approx(30e3)
    assert design.c_yg_f == pytest.approx(9.4e-9)
    assert design.l_cm_required_h == pytest.approx(2.994e-3, rel=1e-3)
    assert design.l_cm_selected_h == 3.3e-3
    assert design.attenuation_cm_db == pytest.approx(40.85, abs=0.05)  # note: "41 dB"
    assert design.l_dm_h == pytest.approx(14.64e-6, rel=1e-3)
    assert design.c_x_required_f == pytest.approx(1.922e-6, rel=1e-3)
    assert design.c_x_selected_f == 2.2e-6
    assert design.attenuation_dm_db == pytest.approx(41.2, abs=0.1)  # note: "41 dB"


def test_two_stage_matches_anp015():
    design = design_line_filter(
        f_sw_hz=300e3,
        a_req_cm_db=40.0,
        a_req_dm_db=40.0,
        c_y_per_line_f=2.2e-9,
        l_dm_h=leakage_inductance_from_impedance(41.0, 1e6),  # 6.5 µH from Z@1 MHz
        stages=2,
        l_cm_candidates=[1e-3, 2.2e-3, 3.3e-3],  # WE-CMB XS availability in the note
        c_x_candidates=[560e-9, 1e-6],
    )
    assert design.f_cutoff_target_hz == pytest.approx(94868.3, rel=1e-4)
    assert design.l_cm_required_h == pytest.approx(0.6396e-3, rel=1e-3)
    assert design.l_cm_selected_h == 1e-3
    assert design.attenuation_cm_db == pytest.approx(47.8, abs=0.1)  # note: "48 dB"
    assert design.c_x_required_f == pytest.approx(431e-9, rel=1e-2)
    assert design.c_x_selected_f == 560e-9
    assert design.attenuation_dm_db == pytest.approx(44.5, abs=0.1)  # note: "45 dB"


def test_cutoff_rejects_met_limit():
    with pytest.raises(ValueError):
        design_line_filter(
            f_sw_hz=300e3,
            a_req_cm_db=-3.0,
            a_req_dm_db=-5.0,
            c_y_per_line_f=4.7e-9,
            l_dm_h=14.6e-6,
            stages=1,
            l_cm_candidates=[3.3e-3],
            c_x_candidates=[2.2e-6],
        )


def test_leakage_current_matches_anp015():
    # Worst case of the note's example: 253 V, 50 Hz, C_Y 5.6 nF, C_X total 2.4 µF -> 0.89 mA.
    current = y_capacitor_leakage_current(253.0, 50.0, 5.6e-9, 5.6e-9, 2.4e-6)
    assert current == pytest.approx(0.889e-3, rel=0.01)


def test_leakage_current_below_typical_limit():
    assert y_capacitor_leakage_current(253.0, 50.0, 5.6e-9, 5.6e-9, 2.4e-6) < 3.5e-3


def test_discharge_resistor_matches_anp015():
    # 2.52 µF total, 60 V touchable within 1 s -> R < 256 kΩ, next standard down 240 kΩ.
    r_max = max_discharge_resistance(2.52e-6, 200.0 * math.sqrt(2.0), 60.0, 1.0)
    assert r_max == pytest.approx(255.9e3, rel=0.01)
    assert round_down_to_series(r_max, E24) == pytest.approx(240e3)
    assert discharge_resistor_power(253.0, 240e3) == pytest.approx(0.267, rel=0.01)


def test_discharge_resistor_rejects_bad_voltages():
    with pytest.raises(ValueError):
        max_discharge_resistance(2.52e-6, 60.0, 60.0, 1.0)
