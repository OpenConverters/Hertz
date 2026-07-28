"""Spectrum-CSV reading — C++ engine (PyHertz).

The engine parses CONTENT; the reference API takes a path, so this reads the
file and prefixes engine messages with it (the reference messages carry the
path, and callers match on them).
"""

from pathlib import Path

import numpy as np

from ._engine import engine

TraceFormatError = engine.TraceFormatError
SpectrumTrace = engine.SpectrumTrace


def _read(path):
    return Path(path).read_text(encoding="utf-8")


def _with_path(path, error):
    return TraceFormatError(f"{path}: {error}")


def spectrum_csv_columns(path):
    """(count, frequency_column, names) of the numeric columns."""
    try:
        columns = engine.spectrum_csv_columns(_read(path))
    except TraceFormatError as error:
        raise _with_path(path, error) from None
    return columns.count, columns.frequency_column, list(columns.names)


def read_spectrum_csv(path, freq_unit=None, level_unit=None, z0_ohm=50.0, level_column=None,
                      return_unit=False):
    """(frequencies_hz, levels[, level_unit]) from a frequency/level export."""
    try:
        trace = engine.parse_spectrum_csv(_read(path), freq_unit, level_unit, z0_ohm, level_column)
    except TraceFormatError as error:
        raise _with_path(path, error) from None
    freqs = np.asarray(trace.frequencies_hz)
    levels = np.asarray(trace.levels_dbuv)
    if return_unit:
        return freqs, levels, trace.level_unit
    return freqs, levels
