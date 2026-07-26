"""CISPR 16-1-1 measuring-receiver emulation on sampled time-domain data.

Method per Krug & Russer, "Quasi-peak detector model for a time-domain
measurement system", IEEE Trans. EMC 47(2), 2005: short-time FFT with a Gaussian
window whose 6 dB bandwidth equals the CISPR resolution bandwidth, per-bin
envelope, then the quasi-peak charge/discharge detector and the critically
damped meter filter as discrete-time one-pole stages.

Readings are calibrated so an unmodulated sine reads its RMS value in dBµV, the
CISPR receiver convention; for a CW tone peak, quasi-peak and average coincide.
"""

import math
from dataclasses import dataclass

import numpy as np

from .utils import dbuv_from_vrms

_LN2 = math.log(2.0)


@dataclass(frozen=True)
class CisprBand:
    """CISPR 16-1-1 band constants: 6 dB RBW and detector time constants."""

    name: str
    f_min_hz: float
    f_max_hz: float
    rbw_6db_hz: float
    tau_charge_s: float
    tau_discharge_s: float
    tau_meter_s: float


BAND_A = CisprBand("A", 9e3, 150e3, 200.0, 45e-3, 500e-3, 160e-3)
BAND_B = CisprBand("B", 150e3, 30e6, 9e3, 1e-3, 160e-3, 160e-3)
BAND_C = CisprBand("C", 30e6, 300e6, 120e3, 1e-3, 550e-3, 100e-3)
BAND_D = CisprBand("D", 300e6, 1e9, 120e3, 1e-3, 550e-3, 100e-3)


def band_for_frequency(f_hz):
    for band in (BAND_A, BAND_B, BAND_C, BAND_D):
        if band.f_min_hz <= f_hz <= band.f_max_hz:
            return band
    raise ValueError(f"{f_hz} Hz is outside CISPR bands A-D")


def stft_envelope(x, fs_hz, band, overlap=0.9, _frames_per_chunk=2048):
    """Per-bin amplitude envelope of x.

    Returns (freqs_hz, times_s, envelope) with envelope shaped
    [n_frames, n_bins]; envelope values are volts of amplitude (a tone at bin
    center of amplitude A yields A).
    """
    x = np.asarray(x, dtype=float)
    if x.ndim != 1:
        raise ValueError("x must be one-dimensional")
    if fs_hz <= 2.0 * band.f_min_hz:
        raise ValueError(f"sample rate {fs_hz} cannot represent band {band.name}")
    if not 0.0 <= overlap < 1.0:
        raise ValueError("overlap must be in [0, 1)")

    sigma_f = band.rbw_6db_hz / (2.0 * math.sqrt(2.0 * _LN2))
    sigma_t = 1.0 / (2.0 * math.pi * sigma_f)
    half = math.ceil(4.0 * sigma_t * fs_hz)
    win_len = 2 * half + 1
    if win_len > x.size:
        raise ValueError(
            f"signal too short: {x.size} samples < one {win_len}-sample window "
            f"(band {band.name} at fs={fs_hz})"
        )
    n = np.arange(win_len) - half
    window = np.exp(-0.5 * (n / (sigma_t * fs_hz)) ** 2)
    window_sum = window.sum()

    hop = max(1, int(round(win_len * (1.0 - overlap))))
    starts = np.arange(0, x.size - win_len + 1, hop)
    nfft = 1 << (win_len - 1).bit_length()

    envelope = np.empty((starts.size, nfft // 2 + 1))
    for begin in range(0, starts.size, _frames_per_chunk):
        block = starts[begin:begin + _frames_per_chunk]
        segments = x[block[:, None] + np.arange(win_len)[None, :]] * window
        envelope[begin:begin + block.size] = 2.0 * np.abs(np.fft.rfft(segments, nfft)) / window_sum

    freqs = np.fft.rfftfreq(nfft, 1.0 / fs_hz)
    times = (starts + half) / fs_hz
    return freqs, times, envelope


def _meter(stream, dt_s, tau_s):
    """Critically damped meter: two cascaded one-pole stages, per-bin max output."""
    alpha = dt_s / tau_s
    m1 = np.zeros(stream.shape[1])
    m2 = np.zeros(stream.shape[1])
    peak = np.zeros(stream.shape[1])
    for row in stream:
        m1 += (row - m1) * alpha
        m2 += (m1 - m2) * alpha
        np.maximum(peak, m2, out=peak)
    return peak


SETTLE_METER_TAUS = 7.0  # dwell until the 2nd-order meter is settled well under 0.1 dB


def _settle_repetitions(n_frames, dt_s, band):
    """How many passes over the envelope the weighting chains need to settle.

    A record shorter than ~5 meter time constants would report the meter
    mid-rise — silently 10-20 dB low on a typical scope capture (a false-PASS
    generator). The record is treated as one period of a stationary signal and
    cycled until the meters settle, exactly as a receiver dwells on it.
    """
    duration = n_frames * dt_s
    needed = SETTLE_METER_TAUS * max(band.tau_meter_s, band.tau_discharge_s / 2.0)
    return max(1, math.ceil(needed / duration))


def quasi_peak_envelope(envelope, dt_s, band):
    """Quasi-peak detector on an envelope stream; returns per-bin volts.

    The envelope is cycled until the charge/discharge + meter chain settles
    (see _settle_repetitions) — the record is assumed stationary/periodic.
    """
    if dt_s >= band.tau_charge_s / 5.0:
        raise ValueError(
            f"envelope sample interval {dt_s:.3g} s too coarse for the "
            f"{band.tau_charge_s:.3g} s charge time constant — raise overlap or fs"
        )
    charge_alpha = dt_s / band.tau_charge_s
    discharge_keep = 1.0 - dt_s / band.tau_discharge_s
    meter_alpha = dt_s / band.tau_meter_s
    detector = np.zeros(envelope.shape[1])
    m1 = np.zeros(envelope.shape[1])
    m2 = np.zeros(envelope.shape[1])
    reading = np.zeros(envelope.shape[1])
    for _ in range(_settle_repetitions(envelope.shape[0], dt_s, band)):
        for row in envelope:
            charging = row > detector
            detector = np.where(
                charging, detector + (row - detector) * charge_alpha, detector * discharge_keep
            )
            m1 += (detector - m1) * meter_alpha
            m2 += (m1 - m2) * meter_alpha
            np.maximum(reading, m2, out=reading)
    return reading


def average_envelope(envelope, dt_s, band):
    meter_alpha = dt_s / band.tau_meter_s
    m1 = np.zeros(envelope.shape[1])
    m2 = np.zeros(envelope.shape[1])
    reading = np.zeros(envelope.shape[1])
    for _ in range(_settle_repetitions(envelope.shape[0], dt_s, band)):
        for row in envelope:
            m1 += (row - m1) * meter_alpha
            m2 += (m1 - m2) * meter_alpha
            np.maximum(reading, m2, out=reading)
    return reading


def peak_envelope(envelope):
    return envelope.max(axis=0)


def measure(x, fs_hz, band, detectors=("peak", "quasi_peak", "average"), overlap=0.9):
    """EMI-receiver readings of a sampled signal.

    Returns {"frequencies_hz": ..., "<detector>_dbuv": ...}; readings are RMS-of-CW
    calibrated dBµV per FFT bin.
    """
    freqs, times, envelope = stft_envelope(x, fs_hz, band, overlap=overlap)
    if times.size < 2:
        raise ValueError("signal yields fewer than two analysis frames")
    dt = float(times[1] - times[0])

    result = {"frequencies_hz": freqs}
    for detector in detectors:
        if detector == "peak":
            volts = peak_envelope(envelope)
        elif detector == "quasi_peak":
            volts = quasi_peak_envelope(envelope, dt, band)
        elif detector == "average":
            volts = average_envelope(envelope, dt, band)
        else:
            raise ValueError(f"unknown detector {detector!r}")
        result[f"{detector}_dbuv"] = dbuv_from_vrms(volts / math.sqrt(2.0))
    return result
