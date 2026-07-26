"""Two-port ABCD chains for in-circuit insertion loss (reference implementation
of cpp/include/hertz/Network.hpp). The asymptotic sizing rule says what to buy;
these curves say what the bought filter actually does between real terminations,
including the CISPR 17 approximate-worst-case 0.1 Ω/100 Ω profiles."""

import math
from dataclasses import dataclass

import numpy as np


@dataclass(frozen=True)
class Abcd:
    a: complex = 1.0
    b: complex = 0.0
    c: complex = 0.0
    d: complex = 1.0


def series_impedance(z):
    return Abcd(1.0, z, 0.0, 1.0)


def shunt_admittance(y):
    return Abcd(1.0, 0.0, y, 1.0)


def cascade(first, second):
    return Abcd(first.a * second.a + first.b * second.c,
                first.a * second.b + first.b * second.d,
                first.c * second.a + first.d * second.c,
                first.c * second.b + first.d * second.d)


def insertion_loss_db(network, z_source, z_load):
    """20·log10 |(A·Zl + B + C·Zs·Zl + D·Zs)/(Zs + Zl)|."""
    numerator = (network.a * z_load + network.b +
                 network.c * z_source * z_load + network.d * z_source)
    return 20.0 * math.log10(abs(numerator / (z_source + z_load)))


def lc_filter_abcd(f_hz, inductance_h, capacitance_f, stages, cap_esl_h=0.0, cap_esr_ohm=0.0):
    """Identical ANP015 stages: series L from the noise source, shunt C after.

    cap_esl_h / cap_esr_ohm: series parasitics of the shunt capacitor (#290) —
    above the cap's self-resonance 1/(2*pi*sqrt(ESL*C)) the branch turns
    inductive and the real filter stops improving. Defaults keep the ideal
    element.
    """
    if f_hz <= 0.0 or inductance_h <= 0.0 or capacitance_f <= 0.0:
        raise ValueError("frequency, inductance and capacitance must be positive")
    if stages not in (1, 2, 3, 4):
        raise ValueError("stages must be 1..4")
    if cap_esl_h < 0.0 or cap_esr_ohm < 0.0:
        raise ValueError("capacitor parasitics must be non-negative")
    w = 2.0 * math.pi * f_hz
    shunt_z = cap_esr_ohm + 1j * (w * cap_esl_h - 1.0 / (w * capacitance_f))
    stage = cascade(series_impedance(1j * w * inductance_h),
                    shunt_admittance(1.0 / shunt_z))
    network = Abcd()
    for _ in range(stages):
        network = cascade(network, stage)
    return network


def insertion_loss_worst_case_db(network):
    """CISPR 17 approximate worst case: min of the 0.1 Ω/100 Ω profiles."""
    return min(insertion_loss_db(network, 0.1, 100.0),
               insertion_loss_db(network, 100.0, 0.1))


def insertion_loss_curves(inductance_h, capacitance_f, stages, reference_impedance_ohm,
                          f_min_hz, f_max_hz, points_per_decade=40):
    """(freqs, standard_db, worst_case_db); reference: 25 Ω CM, 100 Ω DM, 50 Ω data-sheet."""
    if not 0.0 < f_min_hz < f_max_hz or points_per_decade < 2:
        raise ValueError("bad frequency range")
    if reference_impedance_ohm <= 0.0:
        raise ValueError("reference impedance must be positive")
    steps = int((math.log10(f_max_hz) - math.log10(f_min_hz)) * points_per_decade)
    freqs = np.logspace(math.log10(f_min_hz), math.log10(f_max_hz), steps + 1)
    standard = np.empty_like(freqs)
    worst = np.empty_like(freqs)
    for i, f in enumerate(freqs):
        network = lc_filter_abcd(f, inductance_h, capacitance_f, stages)
        standard[i] = insertion_loss_db(network, reference_impedance_ohm, reference_impedance_ohm)
        worst[i] = insertion_loss_worst_case_db(network)
    return freqs, standard, worst


def tabulated_series_il_curves(freqs_hz, z_real_ohm, z_imag_ohm, c_shunt_f, stages,
                               reference_impedance_ohm):
    """Insertion loss using a MEASURED complex series impedance per stage (the
    bound part's impedance curve) against the shunt capacitor — evaluated AT the
    measured frequencies only: no interpolation, no extrapolation, no invented
    points. Returns (standard_db, worst_case_db) arrays aligned with freqs_hz."""
    freqs = np.asarray(freqs_hz, dtype=float)
    z = np.asarray(z_real_ohm, dtype=float) + 1j * np.asarray(z_imag_ohm, dtype=float)
    if freqs.shape != z.shape or freqs.ndim != 1:
        raise ValueError("frequency and impedance arrays must be one-dimensional and equal length")
    if freqs.size < 2 or np.any(np.diff(freqs) <= 0.0):
        raise ValueError("frequencies must be strictly increasing (>= 2 points)")
    if c_shunt_f <= 0.0 or reference_impedance_ohm <= 0.0:
        raise ValueError("shunt capacitance and reference impedance must be positive")
    if stages not in (1, 2, 3, 4):
        raise ValueError("stages must be 1..4")
    standard = np.empty_like(freqs)
    worst = np.empty_like(freqs)
    for i, f in enumerate(freqs):
        w = 2.0 * math.pi * f
        stage = cascade(series_impedance(z[i]), shunt_admittance(1j * w * c_shunt_f))
        network = Abcd()
        for _ in range(stages):
            network = cascade(network, stage)
        standard[i] = insertion_loss_db(network, reference_impedance_ohm, reference_impedance_ohm)
        worst[i] = insertion_loss_worst_case_db(network)
    return standard, worst
