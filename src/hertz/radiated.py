"""Radiated pre-scan SCREENING estimator — the classic electrically-short
common-mode radiator (Ott / Paul):

    |E| = 1.257e-6 * f * L_eff * I / d     [V/m; f Hz, L m, I A, d m]

with the effective length clamped at lambda/4 above the cable's first
resonance. This is the model behind the folklore that ~5 uA of common-mode
current on a 1 m cable fails CISPR 32 Class B at 3 m — and it is a SCREENING
estimate: real installations resonate and couple +-10-20 dB around it. Present
it as triage, never as a measurement.
"""

import math

import numpy as np

RADIATED_MODEL_UNCERTAINTY_DB = 20.0
_C0 = 299792458.0


def radiated_efield_dbuvm(frequencies_hz, cm_current_dbua, cable_length_m, distance_m):
    """dBuV/m per point from a common-mode current spectrum in dBuA."""
    freqs = np.asarray(frequencies_hz, dtype=float)
    dbua = np.asarray(cm_current_dbua, dtype=float)
    if freqs.shape != dbua.shape:
        raise ValueError("frequency and current arrays differ in length")
    if freqs.size == 0:
        raise ValueError("radiated estimate needs at least one point")
    if cable_length_m <= 0.0 or distance_m <= 0.0:
        raise ValueError("cable length and distance must be positive")
    if not (np.all(np.isfinite(freqs)) and np.all(freqs > 0) and np.all(np.isfinite(dbua))):
        raise ValueError("radiated estimate needs positive finite frequencies and finite currents")
    l_eff = np.minimum(cable_length_m, _C0 / (4.0 * freqs))
    i_amp = 10.0 ** (dbua / 20.0) * 1e-6
    e_v_per_m = 1.257e-6 * freqs * l_eff * i_amp / distance_m
    return 20.0 * np.log10(e_v_per_m * 1e6)
