"""Radiated pre-scan screening estimator — C++ engine (PyHertz)."""

import numpy as np

from ._engine import engine

RADIATED_MODEL_UNCERTAINTY_DB = engine.RADIATED_MODEL_UNCERTAINTY_DB
RADIATED_TARGET_MARGIN_DB = engine.RADIATED_TARGET_MARGIN_DB


def radiated_efield_dbuvm(frequencies_hz, cm_current_dbua, cable_length_m, distance_m):
    return np.asarray(engine.radiated_efield_dbuvm(
        np.asarray(frequencies_hz, dtype=float).ravel().tolist(),
        np.asarray(cm_current_dbua, dtype=float).ravel().tolist(),
        cable_length_m, distance_m))


def radiated_cm_attenuation_target_db(frequencies_hz, cm_current_dbua, cable_length_m,
                                      distance_m, limit_dbuvm,
                                      margin_db=RADIATED_TARGET_MARGIN_DB):
    return np.asarray(engine.radiated_cm_attenuation_target_db(
        np.asarray(frequencies_hz, dtype=float).ravel().tolist(),
        np.asarray(cm_current_dbua, dtype=float).ravel().tolist(),
        cable_length_m, distance_m,
        np.asarray(limit_dbuvm, dtype=float).ravel().tolist(), margin_db))
