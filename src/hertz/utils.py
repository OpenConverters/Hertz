"""dB conversions and standard component-value series."""

import math

import numpy as np

E6 = (1.0, 1.5, 2.2, 3.3, 4.7, 6.8)
E12 = (1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2)
E24 = (
    1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
    3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1,
)

_REL_TOL = 1e-9


def dbuv_from_vrms(v_rms):
    """RMS volts to dBµV. Accepts scalars or arrays; zero maps to -inf."""
    v = np.asarray(v_rms, dtype=float)
    if np.any(v < 0.0):
        raise ValueError("RMS voltage must be non-negative")
    with np.errstate(divide="ignore"):
        out = 20.0 * np.log10(v / 1e-6)
    return float(out) if np.isscalar(v_rms) else out


def vrms_from_dbuv(dbuv):
    return 1e-6 * 10.0 ** (np.asarray(dbuv, dtype=float) / 20.0)


def dbuv_from_dbm(dbm, z0_ohm=50.0):
    """Power in dBm across z0_ohm to the voltage it develops, in dBµV (+107 for 50 Ω)."""
    if z0_ohm <= 0.0:
        raise ValueError("reference impedance must be positive")
    return np.asarray(dbm, dtype=float) + 10.0 * math.log10(z0_ohm) + 90.0


def series_candidates(series, decade_min=-15, decade_max=3):
    """All values of a per-decade series expanded over the given decade range."""
    return sorted(s * 10.0**e for e in range(decade_min, decade_max + 1) for s in series)


def round_up_to(value, candidates):
    """Smallest candidate >= value. Raises if none qualifies."""
    if value <= 0.0:
        raise ValueError("value must be positive")
    eligible = [c for c in sorted(candidates) if c >= value * (1.0 - _REL_TOL)]
    if not eligible:
        raise ValueError(f"no candidate >= {value}")
    return eligible[0]


def round_down_to(value, candidates):
    """Largest candidate <= value. Raises if none qualifies."""
    if value <= 0.0:
        raise ValueError("value must be positive")
    eligible = [c for c in sorted(candidates) if c <= value * (1.0 + _REL_TOL)]
    if not eligible:
        raise ValueError(f"no candidate <= {value}")
    return eligible[-1]


def round_up_to_series(value, series=E6):
    exp = math.floor(math.log10(value)) if value > 0 else 0
    return round_up_to(value, [s * 10.0**e for e in (exp, exp + 1) for s in series])


def round_down_to_series(value, series=E6):
    exp = math.floor(math.log10(value)) if value > 0 else 0
    return round_down_to(value, [s * 10.0**e for e in (exp - 1, exp) for s in series])
