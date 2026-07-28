"""Harmonic-comb detection — C++ engine (PyHertz)."""

import numpy as np

from ._engine import engine

MatchedHarmonic = engine.MatchedHarmonic
CombResult = engine.CombResult

PROMINENCE_THRESHOLD_DB = engine.COMB_PROMINENCE_THRESHOLD_DB
MIN_MATCHED_HARMONICS = engine.COMB_MIN_MATCHED_HARMONICS
CONFIDENCE_THRESHOLD = engine.COMB_CONFIDENCE_THRESHOLD


def detect_comb(freqs_hz, levels_dbuv, f_sw_min_hz=20e3, f_sw_max_hz=5e6):
    freqs = np.asarray(freqs_hz, dtype=float)
    levels = np.asarray(levels_dbuv, dtype=float)
    if freqs.ndim != 1 or levels.ndim != 1:
        raise ValueError("frequency and level arrays must be one-dimensional and equal length")
    return engine.detect_comb(freqs.tolist(), levels.tolist(), f_sw_min_hz, f_sw_max_hz)
