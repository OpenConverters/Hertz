import numpy as np
import pytest

from hertz.separation import separate
from hertz.traces import TraceFormatError, read_spectrum_csv, spectrum_csv_columns
from hertz.utils import (
    E6,
    E24,
    dbuv_from_dbm,
    dbuv_from_vrms,
    round_down_to,
    round_up_to,
    round_up_to_series,
    vrms_from_dbuv,
)


def test_separation_recovers_pure_modes():
    t = np.linspace(0.0, 1.0, 1000, endpoint=False)
    cm_true = 0.3 * np.sin(2.0 * np.pi * 5.0 * t)
    dm_true = 0.1 * np.sin(2.0 * np.pi * 9.0 * t)
    cm, dm = separate(cm_true + dm_true, cm_true - dm_true)
    np.testing.assert_allclose(cm, cm_true, atol=1e-12)
    np.testing.assert_allclose(dm, dm_true, atol=1e-12)


def test_separation_shape_mismatch_raises():
    with pytest.raises(ValueError):
        separate(np.zeros(4), np.zeros(5))


def test_dbuv_round_trip():
    assert dbuv_from_vrms(1.0) == pytest.approx(120.0)
    assert vrms_from_dbuv(120.0) == pytest.approx(1.0)
    assert dbuv_from_vrms(0.0) == float("-inf")


def test_dbm_conversion_50_ohm():
    assert float(dbuv_from_dbm(0.0)) == pytest.approx(106.99, abs=0.01)


def test_rounding():
    assert round_up_to_series(2.994e-3, E6) == pytest.approx(3.3e-3)
    assert round_up_to_series(1.922e-6, E6) == pytest.approx(2.2e-6)
    assert round_up_to(0.64e-3, [1e-3, 2.2e-3]) == 1e-3
    assert round_down_to(255.9e3, [220e3, 240e3, 270e3]) == 240e3
    with pytest.raises(ValueError):
        round_up_to(5.0, [1.0, 2.0])
    with pytest.raises(ValueError):
        round_up_to(-1.0, [1.0])


def test_read_csv_with_units_in_header(tmp_path):
    path = tmp_path / "trace.csv"
    path.write_text(
        "Frequency [MHz];Level [dBµV]\n0.15;62.1\n0.5;55.0\n1.0;48.3\n",
        encoding="utf-8",
    )
    freqs, levels = read_spectrum_csv(path)
    np.testing.assert_allclose(freqs, [150e3, 500e3, 1e6])
    np.testing.assert_allclose(levels, [62.1, 55.0, 48.3])


def test_read_csv_dbm_conversion(tmp_path):
    path = tmp_path / "trace.csv"
    path.write_text("Freq (Hz),Amplitude (dBm)\n1000000,-60\n2000000,-70\n", encoding="utf-8")
    freqs, levels = read_spectrum_csv(path)
    assert levels[0] == pytest.approx(46.99, abs=0.01)


def test_read_csv_sorts_by_frequency(tmp_path):
    path = tmp_path / "trace.csv"
    path.write_text("Hz,dBuV\n2e6,40\n1e6,50\n", encoding="utf-8")
    freqs, levels = read_spectrum_csv(path)
    assert freqs.tolist() == [1e6, 2e6]
    assert levels.tolist() == [50.0, 40.0]


def test_read_csv_ambiguous_units_raise(tmp_path):
    path = tmp_path / "trace.csv"
    path.write_text("col1,col2\n0.15,62.1\n0.5,55.0\n", encoding="utf-8")
    with pytest.raises(TraceFormatError):
        read_spectrum_csv(path)
    freqs, _ = read_spectrum_csv(path, freq_unit="MHz", level_unit="dBuV")
    assert freqs[0] == pytest.approx(150e3)


def test_stated_header_unit_beats_override(tmp_path):
    # R-1 at the LIBRARY level: a header stating dBm must not be clobbered by a
    # dBuV override (was a 107 dB error for direct API consumers).
    path = tmp_path / "trace.csv"
    path.write_text("Frequency,Level (dBm)\n0.2,-10\n0.5,-10\n", encoding="utf-8")
    freqs, levels = read_spectrum_csv(path, freq_unit="MHz", level_unit="dBuV")
    assert levels[0] == pytest.approx(96.99, abs=0.01)  # dBm honored, not dBuV
    assert freqs[0] == pytest.approx(200e3)             # override filled the silent axis

def test_multitrace_export_requires_column_choice(tmp_path):
    # F-1 (round 6): a Peak/QP/Average export judged on the silently-taken
    # second column turned a failing QP scan into a passing Average read.
    path = tmp_path / "multitrace.csv"
    path.write_text(
        "Frequency (MHz),Average (dBuV),Quasi-peak (dBuV)\n0.15,50,72\n0.30,47,69\n",
        encoding="utf-8",
    )
    with pytest.raises(TraceFormatError, match="multiple level columns"):
        read_spectrum_csv(path)
    count, names = spectrum_csv_columns(path)
    assert count == 3
    assert names == ["Frequency (MHz)", "Average (dBuV)", "Quasi-peak (dBuV)"]
    freqs, levels = read_spectrum_csv(path, level_column=3)
    assert levels.tolist() == [72.0, 69.0]
    _, avg = read_spectrum_csv(path, level_column=2)
    assert avg.tolist() == [50.0, 47.0]
    with pytest.raises(TraceFormatError, match="out of range"):
        read_spectrum_csv(path, level_column=4)


def test_decimal_comma_semicolon_export(tmp_path):
    # F-3 (round 6): a German-locale ';' export with ',' decimals was shredded
    # into a 0 Hz row by column-count delimiter election.
    path = tmp_path / "de_locale.csv"
    path.write_text(
        "Frequenz [MHz];Pegel [dBuV]\n0,15;72,5\n0,50;62,0\n1,00;60,0\n",
        encoding="utf-8",
    )
    freqs, levels = read_spectrum_csv(path)
    np.testing.assert_allclose(freqs, [150e3, 500e3, 1e6])
    np.testing.assert_allclose(levels, [72.5, 62.0, 60.0])


def test_non_positive_frequency_raises(tmp_path):
    # F-3 (round 6): an f=0 row previously crashed the GUI chart; the parser
    # now refuses it loudly instead of letting garbage through.
    path = tmp_path / "zero_hz.csv"
    path.write_text("Frequency (Hz),Level (dBuV)\n0,10\n150000,72\n500000,62\n", encoding="utf-8")
    with pytest.raises(TraceFormatError, match="non-positive frequency"):
        read_spectrum_csv(path)


def test_nan_level_raises(tmp_path):
    path = tmp_path / "nan.csv"
    path.write_text("Frequency (Hz),Level (dBuV)\n150000,NaN\n500000,62\n", encoding="utf-8")
    with pytest.raises(TraceFormatError, match="non-finite"):
        read_spectrum_csv(path)


def test_preamble_metadata_does_not_conflict_with_stated_column(tmp_path):
    # F-5 (round 6): "RBW 9 kHz" preamble vs a column stating Hz — the column
    # header is the closer context and decides; no false ambiguity, and an
    # override cannot beat the column's stated unit (R-1).
    path = tmp_path / "preamble.csv"
    path.write_text(
        "RBW 9 kHz\nFrequency (Hz),Level (dBuV)\n150000,72\n500000,62\n",
        encoding="utf-8",
    )
    freqs, _ = read_spectrum_csv(path)
    np.testing.assert_allclose(freqs, [150e3, 500e3])
    freqs, _ = read_spectrum_csv(path, freq_unit="MHz")  # override must lose
    np.testing.assert_allclose(freqs, [150e3, 500e3])


def test_preamble_units_still_resolve_unlabelled_columns(tmp_path):
    # when the column header states nothing, the preamble is consulted; two
    # different units there stay ambiguous and need the override
    path = tmp_path / "sweep.csv"
    path.write_text(
        "Start 150 kHz Stop 30 MHz\ncol1,col2\n0.15,62.1\n0.5,55.0\n",
        encoding="utf-8",
    )
    with pytest.raises(TraceFormatError):
        read_spectrum_csv(path, level_unit="dBuV")
    freqs, _ = read_spectrum_csv(path, freq_unit="MHz", level_unit="dBuV")
    assert freqs[0] == pytest.approx(150e3)
