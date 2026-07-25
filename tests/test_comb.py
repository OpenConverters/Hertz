"""Shared golden vectors with the C++ port: the synthetic 300 kHz flyback comb
(the same construction as the web demo scan), a smooth floor, and a lone peak."""

import numpy as np
import pytest

from hertz.comb import detect_comb


def synthetic_comb(f_sw=300e3, n_harmonics=60, peak0_dbuv=84.0, decay_db_per_decade=22.0):
    # Linear grid like a real analyzer export; humps as narrow as a 9 kHz RBW
    # leaves them, so the comb stays resolved across the whole band.
    freqs = np.linspace(150e3, 30e6, 4000)
    levels = 28.0 - 6.0 * np.log10(freqs / 150e3)
    sigma = 9e3
    for n in range(1, n_harmonics + 1):
        fh = n * f_sw
        peak = peak0_dbuv - decay_db_per_decade * np.log10(n)
        levels = np.maximum(levels, peak - 0.5 * ((freqs - fh) / sigma) ** 2)
    return freqs, levels


def smooth_floor():
    freqs = np.logspace(np.log10(150e3), np.log10(30e6), 300)
    return freqs, 40.0 - 8.0 * np.log10(freqs / 150e3)


def test_detects_300khz_comb():
    freqs, levels = synthetic_comb()
    result = detect_comb(freqs, levels)
    assert result.found
    assert result.f_sw_hz == pytest.approx(300e3, rel=0.01)
    assert result.confidence > 0.6
    assert len(result.harmonics) >= 10
    assert result.harmonics[0].order == 1
    assert result.harmonics[0].frequency_hz == pytest.approx(300e3, rel=0.02)


def test_rejects_subharmonic():
    # The winner must be 300 kHz, not 150 kHz (which also matches every peak).
    freqs, levels = synthetic_comb()
    result = detect_comb(freqs, levels)
    assert result.f_sw_hz > 200e3


def test_smooth_floor_finds_nothing():
    freqs, levels = smooth_floor()
    result = detect_comb(freqs, levels)
    assert not result.found
    assert result.f_sw_hz is None


def test_lone_peak_is_not_a_comb():
    freqs, levels = smooth_floor()
    levels = levels + 20.0 * np.exp(-0.5 * ((freqs - 12e6) / 0.2e6) ** 2)
    result = detect_comb(freqs, levels)
    assert not result.found


def test_comb_with_missing_harmonics_still_found():
    freqs, levels = synthetic_comb()
    # notch out harmonics 3 and 4 (e.g. eaten by a resonance)
    for n in (3, 4):
        mask = np.abs(freqs - n * 300e3) < 30e3
        levels[mask] = 28.0 - 6.0 * np.log10(freqs[mask] / 150e3)
    result = detect_comb(freqs, levels)
    assert result.found
    assert result.f_sw_hz == pytest.approx(300e3, rel=0.01)


def test_input_validation():
    with pytest.raises(ValueError):
        detect_comb([1.0, 2.0], [0.0, 0.0])
    freqs, levels = smooth_floor()
    with pytest.raises(ValueError):
        detect_comb(freqs[::-1], levels)
