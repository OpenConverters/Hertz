"""Receiver emulation against REAL data: the CISPR 16-1-1 ed. 4 alternative
calibration waveform (Band A, 100 Hz PRF) from Azpúrua & Hudlička,
DOI 10.5281/zenodo.17779465 (CC-BY-4.0) — see fixtures/ATTRIBUTION.md.

The asserted levels are this project's CHARACTERIZATION of the dataset (not
values stated by the standard): they pin the receiver chain against real
calibration hardware waveforms so any future detector change that shifts them
must justify itself.
"""

import os

import numpy as np
import pytest

from hertz.detector import BAND_A, measure

FIXTURE = os.path.join(os.path.dirname(__file__), "fixtures", "pulse_bandA_100Hz.txt")
FS_HZ = 3e6  # dataset readme: sampling period 1/3 us


@pytest.fixture(scope="module")
def readings():
    period = np.loadtxt(FIXTURE) / 2.0  # open-circuit volts -> 50 ohm load
    assert period.size == 30000  # one 10 ms period at 100 Hz PRF
    signal = np.tile(period, int(2.0 * FS_HZ / period.size))
    result = measure(signal, FS_HZ, BAND_A)
    freqs = result["frequencies_hz"]
    band = (freqs >= 9e3) & (freqs <= 150e3)
    return {d: result[f"{d}_dbuv"][band].max() for d in ("peak", "quasi_peak", "average")}


def test_detectors_ordered_on_real_pulses(readings):
    assert readings["average"] < readings["quasi_peak"] < readings["peak"]


def test_characterized_levels(readings):
    assert readings["peak"] == pytest.approx(66.15, abs=0.3)
    assert readings["quasi_peak"] == pytest.approx(63.6, abs=0.4)
    assert readings["average"] == pytest.approx(59.6, abs=0.4)


def test_quasi_peak_weighting_between_bounds(readings):
    # The whole point of QP: strictly inside the peak/average envelope, and for
    # a 100 Hz Band-A pulse train much closer to peak than to average.
    assert 1.0 < readings["peak"] - readings["quasi_peak"] < 4.0
    assert 2.5 < readings["quasi_peak"] - readings["average"] < 6.0
