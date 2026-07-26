"""Receiver-emulation checks.

CW calibration: a continuous tone must read its RMS level identically on peak,
quasi-peak and average detectors (CISPR receiver convention). Pulsed signal:
average collapses with duty cycle, quasi-peak sits strictly between average and
peak — the property the QP detector exists to provide.
"""

import math

import numpy as np
import pytest

from hertz.detector import BAND_B, band_for_frequency, measure, stft_envelope

TONE_HZ = 300e3
FS_HZ = 1e6
AMPLITUDE = 1e-3  # 1 mV -> 20*log10(1e-3/sqrt(2)/1e-6) = 56.99 dBµV
CW_DBUV = 20.0 * math.log10(AMPLITUDE / math.sqrt(2.0) / 1e-6)


def _tone(duration_s, gate=None):
    t = np.arange(int(duration_s * FS_HZ)) / FS_HZ
    x = AMPLITUDE * np.sin(2.0 * math.pi * TONE_HZ * t)
    if gate is not None:
        x = x * gate(t)
    return x


def test_band_lookup():
    assert band_for_frequency(TONE_HZ) is BAND_B
    with pytest.raises(ValueError):
        band_for_frequency(2e9)


def test_envelope_calibration_cw():
    freqs, times, env = stft_envelope(_tone(0.02), FS_HZ, BAND_B)
    peak_bin = env.max(axis=0).argmax()
    assert freqs[peak_bin] == pytest.approx(TONE_HZ, abs=freqs[1] - freqs[0])
    # Worst-case off-bin scalloping with the Gaussian RBW window is < 0.2 dB.
    assert env.max() == pytest.approx(AMPLITUDE, rel=0.03)


def test_cw_reads_rms_on_all_detectors():
    result = measure(_tone(1.0), FS_HZ, BAND_B)
    for detector in ("peak", "quasi_peak", "average"):
        reading = result[f"{detector}_dbuv"].max()
        assert reading == pytest.approx(CW_DBUV, abs=0.45), detector


def test_pulsed_signal_orders_detectors():
    gate = lambda t: ((t * 100.0) % 1.0 < 0.1).astype(float)  # 100 Hz PRF, 10 % duty
    result = measure(_tone(1.0, gate=gate), FS_HZ, BAND_B)
    bin_at_tone = np.abs(result["frequencies_hz"] - TONE_HZ).argmin()
    pk = result["peak_dbuv"][bin_at_tone]
    qp = result["quasi_peak_dbuv"][bin_at_tone]
    avg = result["average_dbuv"][bin_at_tone]
    assert avg < qp < pk
    assert pk - avg == pytest.approx(20.0, abs=1.5)  # envelope mean = duty cycle
    assert pk - qp < 3.0  # 1 ms charge vs 160 ms discharge holds QP near peak


def test_signal_too_short_raises():
    with pytest.raises(ValueError):
        stft_envelope(_tone(0.0001), FS_HZ, BAND_B)


def test_unknown_detector_raises():
    with pytest.raises(ValueError):
        measure(_tone(0.02), FS_HZ, BAND_B, detectors=("median",))


def test_short_record_settles_by_cycling():
    # A 120 ms capture used to read the meter mid-rise (~18 dB low). The
    # detectors now dwell on the cycled record until settled.
    result = measure(_tone(0.12), FS_HZ, BAND_B)
    for detector in ("quasi_peak", "average"):
        assert result[f"{detector}_dbuv"].max() == pytest.approx(CW_DBUV, abs=0.45), detector
