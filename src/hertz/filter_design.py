"""Single-phase line-filter design per Würth Elektronik application note ANP015
("Line filter design for single-phase applications"). Equation numbers in
comments refer to ANP015 rev. 2024-06.

The procedure sizes the common-mode stage (CM choke against 25 Ω LISN CM path
with the Y capacitors) and the differential-mode stage (choke leakage inductance
against 100 Ω with the X capacitor). Component values are then rounded UP onto
an explicit candidate list — a catalog availability list or a standard E-series;
there is no built-in default catalog on purpose.
"""

import math
from dataclasses import dataclass

import numpy as np

from .utils import round_up_to

MIN_MEASURED_FREQUENCY_HZ = 150e3  # conducted-emission bands start here


def design_frequency(f_sw_hz):
    """Frequency the filter is designed at: f_sw, or its first harmonic >= 150 kHz."""
    if f_sw_hz <= 0.0:
        raise ValueError("switching frequency must be positive")
    if f_sw_hz >= MIN_MEASURED_FREQUENCY_HZ:
        return f_sw_hz
    return math.ceil(MIN_MEASURED_FREQUENCY_HZ / f_sw_hz) * f_sw_hz


def required_attenuation_db(measured_dbuv, limit_dbuv, margin_db=10.0):
    """A_req = measured - limit + margin (ANP015 §1; 10 dB is the note's buffer).

    Negative results mean the limit is already met at that frequency.
    """
    return np.asarray(measured_dbuv, dtype=float) - np.asarray(limit_dbuv, dtype=float) + margin_db


def cutoff_frequency(f_design_hz, a_req_db, stages):
    """f_CO = f_design / 10^(A/(40·n)) — eqs. (4) single stage, (11) two stage.

    A of exactly 0 is legal (resonance AT the design frequency suffices — the
    mode needs no headroom); a NEGATIVE requirement means no filter at all.
    """
    if stages not in (1, 2):
        raise ValueError("ANP015 covers 1- or 2-stage filters")
    if a_req_db < 0.0:
        raise ValueError("required attenuation is negative — the limit is already met")
    return f_design_hz / 10.0 ** (a_req_db / (40.0 * stages))


def resonant_cutoff(inductance_h, capacitance_f):
    if inductance_h <= 0.0 or capacitance_f <= 0.0:
        raise ValueError("inductance and capacitance must be positive")
    return 1.0 / (2.0 * math.pi * math.sqrt(inductance_h * capacitance_f))


def cm_inductance(f_cutoff_hz, c_yg_f):
    """L_CM = 1/((2π f_CO)² C_YG) — eq. (7); C_YG = 2·C_Y per line pair, eq. (6)."""
    if f_cutoff_hz <= 0.0 or c_yg_f <= 0.0:
        raise ValueError("cutoff frequency and C_YG must be positive")
    return 1.0 / ((2.0 * math.pi * f_cutoff_hz) ** 2 * c_yg_f)


def dm_capacitance(f_cutoff_hz, l_dm_h):
    """C_X = 1/((2π f_CO)² L_DM) — eq. (9)."""
    if f_cutoff_hz <= 0.0 or l_dm_h <= 0.0:
        raise ValueError("cutoff frequency and L_DM must be positive")
    return 1.0 / ((2.0 * math.pi * f_cutoff_hz) ** 2 * l_dm_h)


def leakage_inductance_from_impedance(z_mag_ohm, f_hz):
    """L from the inductive region of a measured DM impedance curve — eq. (8)."""
    if z_mag_ohm <= 0.0 or f_hz <= 0.0:
        raise ValueError("impedance and frequency must be positive")
    return z_mag_ohm / (2.0 * math.pi * f_hz)


def achieved_attenuation_db(f_design_hz, inductance_h, capacitance_f, stages):
    """A = 40·n·log10(f_design/f_CO) with the actually selected L and C.

    Asymptotic slope estimate; negative when the resonance sits above f_design.
    """
    if stages not in (1, 2):
        raise ValueError("ANP015 covers 1- or 2-stage filters")
    f_co = resonant_cutoff(inductance_h, capacitance_f)
    return 40.0 * stages * math.log10(f_design_hz / f_co)


def x_capacitor_dm_factor(n_lines):
    """X-capacitor connection factor seen by a line-to-line DM loop.

    A 3-wire (delta) X network presents C + C/2 = 1.5 C between any two lines;
    the line-neutral (2-wire) and phase-neutral star (4-wire) networks present
    the capacitor directly.
    """
    if n_lines not in (2, 3, 4):
        raise ValueError("n_lines must be 2 (1-phase/DC), 3 (3-phase) or 4 (3-phase + N)")
    return 1.5 if n_lines == 3 else 1.0


@dataclass(frozen=True)
class LineFilterDesign:
    stages: int
    f_design_cm_hz: float
    f_design_dm_hz: float
    f_cutoff_cm_hz: float
    f_cutoff_dm_hz: float
    c_y_per_line_f: float
    c_yg_f: float
    l_cm_required_h: float
    l_cm_selected_h: float
    attenuation_cm_db: float
    l_dm_h: float
    c_x_required_f: float
    c_x_selected_f: float
    attenuation_dm_db: float
    n_lines: int = 2
    c_x_dm_factor: float = 1.0


def design_line_filter(
    f_sw_hz,
    a_req_cm_db,
    a_req_dm_db,
    c_y_per_line_f,
    l_dm_h,
    stages,
    l_cm_candidates,
    c_x_candidates,
    f_design_cm_hz=None,
    f_design_dm_hz=None,
    n_lines=2,
):
    """Full ANP015 sizing pass.

    c_y_per_line_f: the chosen Y capacitor per line (bounded by leakage current,
    see y_capacitor_leakage_current). l_dm_h: DM (leakage) inductance of the CM
    choke. Candidate lists hold the actually available component values; both
    stages of a 2-stage filter use identical components (ANP015 §3).

    CM and DM are PHYSICALLY INDEPENDENT networks (choke vs. Y caps; leakage
    vs. X cap), so each gets its own cutoff from ITS OWN requirement at ITS
    OWN design frequency — collapsing to max(A_cm, A_dm) sized the quiet mode
    from the loud mode's problem. The optional per-mode design frequencies
    serve measurement-driven callers whose CM and DM critical points sit at
    different frequencies; both default to design_frequency(f_sw_hz).
    """
    f_design_default = design_frequency(f_sw_hz)
    f_design_cm = f_design_default if f_design_cm_hz is None else f_design_cm_hz
    f_design_dm = f_design_default if f_design_dm_hz is None else f_design_dm_hz
    if f_design_cm <= 0.0 or f_design_dm <= 0.0:
        raise ValueError("per-mode design frequencies must be positive")
    f_cutoff_cm = cutoff_frequency(f_design_cm, a_req_cm_db, stages)
    f_cutoff_dm = cutoff_frequency(f_design_dm, a_req_dm_db, stages)

    x_factor = x_capacitor_dm_factor(n_lines)
    c_yg = n_lines * c_y_per_line_f
    l_cm_required = cm_inductance(f_cutoff_cm, c_yg)
    l_cm_selected = round_up_to(l_cm_required, l_cm_candidates)
    attenuation_cm = achieved_attenuation_db(f_design_cm, l_cm_selected, c_yg, stages)

    c_x_required = dm_capacitance(f_cutoff_dm, l_dm_h) / x_factor
    c_x_selected = round_up_to(c_x_required, c_x_candidates)
    attenuation_dm = achieved_attenuation_db(f_design_dm, l_dm_h, x_factor * c_x_selected, stages)

    return LineFilterDesign(
        stages=stages,
        f_design_cm_hz=f_design_cm,
        f_design_dm_hz=f_design_dm,
        f_cutoff_cm_hz=f_cutoff_cm,
        f_cutoff_dm_hz=f_cutoff_dm,
        c_y_per_line_f=c_y_per_line_f,
        c_yg_f=c_yg,
        l_cm_required_h=l_cm_required,
        l_cm_selected_h=l_cm_selected,
        attenuation_cm_db=attenuation_cm,
        l_dm_h=l_dm_h,
        c_x_required_f=c_x_required,
        c_x_selected_f=c_x_selected,
        attenuation_dm_db=attenuation_dm,
        n_lines=n_lines,
        c_x_dm_factor=x_factor,
    )


def y_capacitor_leakage_current(v_grid_rms, f_grid_hz, c_y_line_f, c_y_neutral_f, c_x_total_f):
    """Protective-earth leakage current — ANP015 eqs. (29)-(30).

    Apply worst-case tolerances (e.g. V+10 %, C+20 %) in the arguments; this
    function computes the plain formula only.
    """
    if min(v_grid_rms, f_grid_hz) <= 0.0 or min(c_y_line_f, c_y_neutral_f) < 0.0:
        raise ValueError("grid voltage/frequency must be positive, capacitances non-negative")
    series = c_x_total_f * c_y_neutral_f / (c_x_total_f + c_y_neutral_f) if c_x_total_f > 0.0 else 0.0
    return v_grid_rms * 2.0 * math.pi * f_grid_hz * (c_y_line_f + series)


def max_discharge_resistance(c_total_f, v_start, v_safe, t_max_s):
    """Largest X-cap discharge resistor meeting v(t_max) <= v_safe — eq. (34)."""
    if not 0.0 < v_safe < v_start:
        raise ValueError("need 0 < v_safe < v_start")
    if c_total_f <= 0.0 or t_max_s <= 0.0:
        raise ValueError("capacitance and time must be positive")
    return t_max_s / (c_total_f * math.log(v_start / v_safe))


def discharge_resistor_power(v_grid_rms, resistance_ohm):
    """Continuous dissipation of the discharge resistor — eq. (35)."""
    if resistance_ohm <= 0.0:
        raise ValueError("resistance must be positive")
    return v_grid_rms**2 / resistance_ohm


@dataclass(frozen=True)
class FilterInteraction:
    resonance_hz: float
    characteristic_impedance_ohm: float
    converter_input_impedance_ohm: float
    margin_db: float                 # 20·log10(Zin/R0): >= 12 dB comfortable, < 6 dB critical
    damping_resistor_ohm: float
    damping_capacitor_min_f: float
    damping_capacitor_max_f: float


def input_filter_interaction(inductance_h, capacitance_f, v_in_min, p_in):
    """Middlebrook check: the filter's undamped output-impedance peak (R0 at
    resonance) must sit well below the converter's negative-incremental input
    impedance |Z_in| = V_in(min)²/P_in. Damping per the Trilogy: R_d = √(L/C)
    (Q = 1), C_d = 5–10 × C."""
    if inductance_h <= 0.0 or capacitance_f <= 0.0:
        raise ValueError("inductance and capacitance must be positive")
    if v_in_min <= 0.0 or p_in <= 0.0:
        raise ValueError("minimum input voltage and input power must be positive")
    characteristic = math.sqrt(inductance_h / capacitance_f)
    converter_input = v_in_min**2 / p_in
    return FilterInteraction(
        resonance_hz=resonant_cutoff(inductance_h, capacitance_f),
        characteristic_impedance_ohm=characteristic,
        converter_input_impedance_ohm=converter_input,
        margin_db=20.0 * math.log10(converter_input / characteristic),
        damping_resistor_ohm=characteristic,
        damping_capacitor_min_f=5.0 * capacitance_f,
        damping_capacitor_max_f=10.0 * capacitance_f,
    )
