"""Mirrors cpp/tests/test_comb_network.cpp network + Middlebrook cases."""

import cmath
import math

import pytest

from hertz.filter_design import input_filter_interaction, resonant_cutoff
from hertz.network import (
    insertion_loss_curves,
    insertion_loss_db,
    insertion_loss_worst_case_db,
    lc_filter_abcd,
)

L, C = 14.64e-6, 2.2e-6  # the ANP015 single-stage DM values


def direct_node_analysis_il(f, zs, zl):
    w = 2.0 * math.pi * f
    z_l = 1j * w * L
    z_c = -1j / (w * C)
    z_p = z_c * zl / (z_c + zl)
    return 20.0 * math.log10(abs((zl / (zs + zl)) / (z_p / (zs + z_l + z_p))))


def test_abcd_matches_direct_node_analysis():
    for f in (10e3, 100e3, 300e3, 1e6, 10e6):
        for zs in (0.1, 50.0, 100.0):
            for zl in (0.1, 50.0, 100.0):
                network = lc_filter_abcd(f, L, C, 1)
                assert insertion_loss_db(network, zs, zl) == pytest.approx(
                    direct_node_analysis_il(f, zs, zl), abs=1e-9)


def test_asymptotic_slopes_above_both_corners():
    at1 = lambda f: insertion_loss_db(lc_filter_abcd(f, L, C, 1), 100.0, 100.0)
    at2 = lambda f: insertion_loss_db(lc_filter_abcd(f, L, C, 2), 100.0, 100.0)
    assert at1(30e6) - at1(3e6) == pytest.approx(40.0, abs=1.0)
    assert at2(30e6) - at2(3e6) == pytest.approx(80.0, abs=1.5)


def test_worst_case_never_beats_symmetric_reference():
    f0 = resonant_cutoff(L, C)
    for f in (3.0 * f0, 10.0 * f0, 30.0 * f0):
        network = lc_filter_abcd(f, L, C, 1)
        assert insertion_loss_worst_case_db(network) <= \
            insertion_loss_db(network, 100.0, 100.0) + 1e-9


def test_curves_consistent_with_point_evaluation():
    freqs, standard, worst = insertion_loss_curves(L, C, 1, 100.0, 150e3, 30e6, 20)
    mid = len(freqs) // 2
    network = lc_filter_abcd(freqs[mid], L, C, 1)
    assert standard[mid] == pytest.approx(insertion_loss_db(network, 100.0, 100.0), abs=1e-9)
    assert len(freqs) == len(standard) == len(worst)


def test_middlebrook_interaction():
    interaction = input_filter_interaction(L, C, 207.0, 25.0)
    assert interaction.characteristic_impedance_ohm == pytest.approx(2.579, rel=1e-3)
    assert interaction.converter_input_impedance_ohm == pytest.approx(1713.96, rel=1e-3)
    assert interaction.margin_db == pytest.approx(56.45, abs=0.05)
    assert interaction.damping_resistor_ohm == pytest.approx(2.579, rel=1e-3)
    assert interaction.damping_capacitor_min_f == pytest.approx(11e-6)
    assert interaction.damping_capacitor_max_f == pytest.approx(22e-6)
    tight = input_filter_interaction(L, C, 36.0, 500.0)
    assert tight.margin_db == pytest.approx(0.05, abs=0.06)
    with pytest.raises(ValueError):
        input_filter_interaction(0.0, C, 48.0, 100.0)


def test_validation():
    with pytest.raises(ValueError):
        lc_filter_abcd(1e6, L, C, 5)
    with pytest.raises(ValueError):
        insertion_loss_curves(L, C, 1, -1.0, 150e3, 30e6)
