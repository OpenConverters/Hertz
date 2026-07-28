"""Two-port ABCD chains — C++ engine (PyHertz)."""

import numpy as np

from ._engine import engine

Abcd = engine.Abcd
series_impedance = engine.series_impedance
shunt_admittance = engine.shunt_admittance
cascade = engine.cascade
insertion_loss_db = engine.insertion_loss_db
lc_filter_abcd = engine.lc_filter_abcd
insertion_loss_worst_case_db = engine.insertion_loss_worst_case_db


def insertion_loss_curves(inductance_h, capacitance_f, stages, reference_impedance_ohm,
                          f_min_hz, f_max_hz, points_per_decade=40):
    curves = engine.insertion_loss_curves(inductance_h, capacitance_f, stages,
                                          reference_impedance_ohm, f_min_hz, f_max_hz,
                                          points_per_decade)
    return (np.asarray(curves.frequencies_hz), np.asarray(curves.standard_db),
            np.asarray(curves.worst_case_db))


def tabulated_series_il_curves(freqs_hz, z_real_ohm, z_imag_ohm, c_shunt_f, stages,
                               reference_impedance_ohm):
    curves = engine.tabulated_series_il_curves(
        np.asarray(freqs_hz, dtype=float).tolist(), np.asarray(z_real_ohm, dtype=float).tolist(),
        np.asarray(z_imag_ohm, dtype=float).tolist(), c_shunt_f, stages, reference_impedance_ohm)
    return np.asarray(curves.standard_db), np.asarray(curves.worst_case_db)
