"""Spectrum-analyzer trace ingestion.

Reads two-column frequency/level CSV exports (Rigol, Siglent, Tekbox EMCview,
R&S, Tektronix all emit a variant of this). Units are taken from the header
row; if the header does not state them they MUST be passed explicitly —
guessing units on EMI data is how 60 dB mistakes happen, so ambiguity raises.
"""

import re

import numpy as np

from .utils import dbuv_from_dbm

_FREQ_UNITS = {"hz": 1.0, "khz": 1e3, "mhz": 1e6, "ghz": 1e9}
_LEVEL_UNITS = ("dbuv", "dbµv", "dbμv", "dbm")
_DELIMITERS = (",", ";", "\t")


class TraceFormatError(ValueError):
    """The trace file cannot be interpreted unambiguously."""


def _split(line, delimiter):
    return [tok.strip() for tok in line.split(delimiter)]


def _sniff_delimiter(lines):
    best, best_count = None, 1
    for d in _DELIMITERS:
        count = max(len(_split(line, d)) for line in lines)
        if count > best_count:
            best, best_count = d, count
    if best is None:
        raise TraceFormatError("no recognizable column delimiter (',', ';' or tab)")
    return best


def _find_unit(text, candidates):
    lowered = text.lower()
    hits = [u for u in candidates if re.search(rf"(?<![a-z0-9]){re.escape(u)}(?![a-z0-9])", lowered)]
    if len(hits) > 1 and len(set(hits) - {"dbµv", "dbμv", "dbuv"}) < len(hits):
        hits = sorted(set("dbuv" if h in ("dbµv", "dbμv") else h for h in hits))
    if len(hits) == 1:
        return hits[0]
    if len(hits) > 1:
        raise TraceFormatError(f"conflicting units in header: {hits}")
    return None


def read_spectrum_csv(path, freq_unit=None, level_unit=None, z0_ohm=50.0):
    """(frequencies_hz, levels_dbuv) from a two-column trace export.

    freq_unit: 'Hz'|'kHz'|'MHz'|'GHz', level_unit: 'dBuV'|'dBm' — only needed
    when the file header does not state them.
    """
    with open(path, encoding="utf-8-sig") as fh:
        lines = [line.rstrip("\n") for line in fh if line.strip()]
    if not lines:
        raise TraceFormatError(f"{path}: empty file")

    delimiter = _sniff_delimiter(lines[:20])
    header_text = ""
    rows = []
    for line in lines:
        tokens = _split(line, delimiter)
        numeric = []
        for tok in tokens:
            try:
                numeric.append(float(tok))
            except ValueError:
                continue
        if len(numeric) >= 2:
            rows.append((numeric[0], numeric[1]))
        elif not rows:
            header_text += " " + line

    if len(rows) < 2:
        raise TraceFormatError(f"{path}: fewer than two data rows found")

    file_freq_unit = _find_unit(header_text, tuple(_FREQ_UNITS))
    file_level_unit = _find_unit(header_text, _LEVEL_UNITS)
    freq_unit = (freq_unit or file_freq_unit or "").lower()
    level_unit = (level_unit or file_level_unit or "").lower()
    if level_unit in ("dbµv", "dbμv"):
        level_unit = "dbuv"
    if freq_unit not in _FREQ_UNITS:
        raise TraceFormatError(
            f"{path}: frequency unit not stated in header — pass freq_unit explicitly"
        )
    if level_unit not in ("dbuv", "dbm"):
        raise TraceFormatError(
            f"{path}: level unit not stated in header — pass level_unit explicitly"
        )

    data = np.asarray(rows, dtype=float)
    freqs = data[:, 0] * _FREQ_UNITS[freq_unit]
    levels = data[:, 1]
    if level_unit == "dbm":
        levels = dbuv_from_dbm(levels, z0_ohm=z0_ohm)
    order = np.argsort(freqs)
    return freqs[order], levels[order]
