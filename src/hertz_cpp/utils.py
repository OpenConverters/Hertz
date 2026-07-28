"""dB conversions and component-value series — C++ engine (PyHertz).

Only shape adaptation lives here: the reference API accepts arrays where the
engine is scalar, so each element goes through the SAME engine call. No formula
is reimplemented in Python; `series_candidates` has no engine counterpart and is
deliberately absent rather than faked.
"""

import numpy as np

from ._engine import engine

E6 = tuple(engine.E6)
E12 = tuple(engine.E12)
E24 = tuple(engine.E24)


def _elementwise(fn, values):
    array = np.asarray(values, dtype=float)
    if array.ndim == 0:
        return fn(float(array))
    flat = np.array([fn(float(v)) for v in array.ravel()])
    return flat.reshape(array.shape)


def dbuv_from_vrms(v_rms):
    return _elementwise(engine.dbuv_from_vrms, v_rms)


def vrms_from_dbuv(dbuv):
    return _elementwise(engine.vrms_from_dbuv, dbuv)


def dbuv_from_dbm(dbm, z0_ohm=50.0):
    return _elementwise(lambda v: engine.dbuv_from_dbm(v, z0_ohm), dbm)


def series_candidates(series, decade_min=-15, decade_max=3):
    """All values of a per-decade series expanded over the given decade range.

    Candidate-list bookkeeping with no engine counterpart — no physics, and
    identical to the reference by construction (the differential sweep pins it).
    """
    return sorted(s * 10.0**e for e in range(decade_min, decade_max + 1) for s in series)


def round_up_to(value, candidates):
    return engine.round_up_to(value, list(candidates))


def round_down_to(value, candidates):
    return engine.round_down_to(value, list(candidates))


def round_up_to_series(value, series=E6):
    return engine.round_up_to_series(value, list(series))


def round_down_to_series(value, series=E6):
    return engine.round_down_to_series(value, list(series))
