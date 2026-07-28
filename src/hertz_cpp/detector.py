"""CISPR 16-1-1 receiver emulation — C++ engine (PyHertz).

`measure` computes all three detectors in one engine pass; the `detectors`
argument of the reference API only selects which of them are returned (and
which names are legal).
"""

import numpy as np

from ._engine import engine

CisprBand = engine.CisprBand
BAND_A = engine.BAND_A
BAND_B = engine.BAND_B
BAND_C = engine.BAND_C
BAND_D = engine.BAND_D
band_for_frequency = engine.band_for_frequency
SETTLE_METER_TAUS = engine.SETTLE_METER_TAUS

_READING_ATTRIBUTE = {
    "peak": "peak_dbuv",
    "quasi_peak": "quasi_peak_dbuv",
    "average": "average_dbuv",
}


def stft_envelope(x, fs_hz, band, overlap=0.9):
    """(freqs_hz, times_s, envelope[n_frames, n_bins])."""
    freqs, times, envelope = engine.stft_envelope(
        np.asarray(x, dtype=float).ravel().tolist(), fs_hz, band, overlap)
    return np.asarray(freqs), np.asarray(times), envelope


def measure(x, fs_hz, band, detectors=("peak", "quasi_peak", "average"), overlap=0.9):
    """EMI-receiver readings of a sampled signal, RMS-of-CW calibrated dBuV."""
    for detector in detectors:
        if detector not in _READING_ATTRIBUTE:
            raise ValueError(f"unknown detector {detector!r}")
    reading = engine.measure(np.asarray(x, dtype=float).ravel().tolist(), fs_hz, band, overlap)
    result = {"frequencies_hz": np.asarray(reading.frequencies_hz)}
    for detector in detectors:
        result[f"{detector}_dbuv"] = np.asarray(getattr(reading, _READING_ATTRIBUTE[detector]))
    return result
