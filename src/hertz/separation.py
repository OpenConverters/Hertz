"""Common-mode / differential-mode separation.

Exact separation needs the two LISN line voltages as synchronized time-domain
samples or complex spectra: V_CM = (V_L + V_N)/2, V_DM = (V_L - V_N)/2
(ANP015 eqs. 1-2). It is NOT possible from two magnitude-only spectra — the
relative phase is gone — so this module never accepts them; feeding magnitude
spectra here produces garbage, which is why no such convenience path exists.
"""

import numpy as np


def separate(v_line, v_neutral):
    """(common_mode, differential_mode) from the two line quantities."""
    a = np.asarray(v_line)
    b = np.asarray(v_neutral)
    if a.shape != b.shape:
        raise ValueError(f"shape mismatch: {a.shape} vs {b.shape}")
    if a.size == 0:
        raise ValueError("empty input")
    return (a + b) / 2.0, (a - b) / 2.0
