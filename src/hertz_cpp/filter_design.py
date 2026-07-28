"""ANP015 line-filter design — C++ engine (PyHertz)."""

import numpy as np

from ._engine import engine

MIN_MEASURED_FREQUENCY_HZ = engine.MIN_MEASURED_FREQUENCY_HZ

LineFilterDesign = engine.LineFilterDesign
FilterInteraction = engine.FilterInteraction
GoverningRequirement = engine.GoverningRequirement

design_frequency = engine.design_frequency
cutoff_frequency = engine.cutoff_frequency
resonant_cutoff = engine.resonant_cutoff
cm_inductance = engine.cm_inductance
dm_capacitance = engine.dm_capacitance
leakage_inductance_from_impedance = engine.leakage_inductance_from_impedance
achieved_attenuation_db = engine.achieved_attenuation_db
x_capacitor_dm_factor = engine.x_capacitor_dm_factor
y_capacitor_leakage_current = engine.y_capacitor_leakage_current
max_discharge_resistance = engine.max_discharge_resistance
discharge_resistor_power = engine.discharge_resistor_power
input_filter_interaction = engine.input_filter_interaction
governing_requirement = engine.governing_requirement


def required_attenuation_db(measured_dbuv, limit_dbuv, margin_db=10.0):
    """A_req = measured - limit + margin; arrays go through per point."""
    measured = np.asarray(measured_dbuv, dtype=float)
    limit = np.asarray(limit_dbuv, dtype=float)
    if measured.ndim == 0 and limit.ndim == 0:
        return engine.required_attenuation_db(float(measured), float(limit), margin_db)
    measured, limit = np.broadcast_arrays(measured, limit)
    flat = np.array([engine.required_attenuation_db(float(m), float(l), margin_db)
                     for m, l in zip(measured.ravel(), limit.ravel())])
    return flat.reshape(measured.shape)


def design_line_filter(f_sw_hz, a_req_cm_db, a_req_dm_db, c_y_per_line_f, l_dm_h, stages,
                       l_cm_candidates, c_x_candidates, f_design_cm_hz=None,
                       f_design_dm_hz=None, n_lines=2):
    return engine.design_line_filter(f_sw_hz, a_req_cm_db, a_req_dm_db, c_y_per_line_f, l_dm_h,
                                     stages, list(l_cm_candidates), list(c_x_candidates),
                                     f_design_cm_hz, f_design_dm_hz, n_lines)
