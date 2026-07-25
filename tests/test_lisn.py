import numpy as np
import pytest

from hertz.lisn import cispr16_lisn, cispr25_lisn


def test_cispr16_high_frequency_asymptote_is_50_ohm():
    lisn = cispr16_lisn()
    assert abs(lisn.eut_impedance(30e6)) == pytest.approx(50.0, rel=0.03)


def test_cispr16_band_edge_impedance():
    # CISPR 16 nominal |Z| at 150 kHz is ~35-40 Ω (tolerance band ±20 %).
    z = abs(cispr16_lisn().eut_impedance(150e3))
    assert 30.0 < z < 45.0


def test_cispr16_impedance_monotonic_rise():
    freqs = np.logspace(np.log10(150e3), np.log10(30e6), 40)
    mags = np.abs(cispr16_lisn().eut_impedance(freqs))
    assert np.all(np.diff(mags) > 0.0)
    assert np.all(mags < 50.5)


def test_cispr25_5uh_lower_impedance_at_low_frequency():
    z25 = abs(cispr25_lisn().eut_impedance(150e3))
    z16 = abs(cispr16_lisn().eut_impedance(150e3))
    assert z25 < z16
    assert abs(cispr25_lisn().eut_impedance(108e6)) == pytest.approx(50.0, rel=0.03)


def test_rejects_nonpositive_frequency():
    with pytest.raises(ValueError):
        cispr16_lisn().eut_impedance(0.0)


def test_spice_subckt_contains_elements():
    text = cispr16_lisn().to_spice_subckt(name="LISN50")
    assert ".subckt LISN50 eut mains meas" in text
    assert "L1 eut mains 5e-05" in text
    assert "C1 eut meas 1e-07" in text
    assert "R1 meas 0 50" in text
    assert text.strip().endswith(".ends LISN50")


def test_spice_subckt_unterminated():
    text = cispr25_lisn().to_spice_subckt(name="LISN5", terminated=False)
    assert "R1" not in text
