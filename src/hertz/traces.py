"""Spectrum-analyzer trace ingestion.

Reads frequency/level CSV exports (Rigol, Siglent, Tekbox EMCview, R&S,
Tektronix all emit a variant of this). Units are taken from the header row; if
the header does not state them they MUST be passed explicitly — guessing units
on EMI data is how 60 dB mistakes happen, so ambiguity raises. The same
zero-guessing rule applies to multi-trace exports (Peak/QP/Average side by
side, the normal receiver format): with more than two numeric columns the
caller MUST say which column to judge — silently taking the second one turned
a failing QP scan into a passing Average read.
"""

import math
import re
from dataclasses import dataclass

import numpy as np

from .utils import dbuv_from_dbm

_FREQ_UNITS = {"hz": 1.0, "khz": 1e3, "mhz": 1e6, "ghz": 1e9}
_LEVEL_UNITS = ("dbuv", "dbµv", "dbμv", "dbm")
_DELIMITERS = (";", "\t", ",")  # preference order — see _sniff_delimiter


class TraceFormatError(ValueError):
    """The trace file cannot be interpreted unambiguously."""


def _split(line, delimiter):
    return [tok.strip() for tok in line.split(delimiter)]


def _sniff_delimiter(lines):
    # Preference order, not raw column count: a European ';' export carries
    # decimal commas, and counting columns would elect ',' and shred "0,15"
    # into a 0 Hz row. ';' and tab can only be delimiters on purpose.
    for delimiter in _DELIMITERS:
        if max(len(_split(line, delimiter)) for line in lines) >= 2:
            return delimiter
    raise TraceFormatError("no recognizable column delimiter (',', ';' or tab)")


def _parse_field(token, delimiter):
    """float or None; with European-locale support: when the delimiter is NOT
    the comma, a token like "72,5" is a decimal-comma number (German analyzer
    exports are ';'-delimited with ',' decimals)."""
    try:
        return float(token)
    except ValueError:
        pass
    if delimiter != "," and "." not in token and token.count(",") == 1:
        try:
            return float(token.replace(",", "."))
        except ValueError:
            return None
    return None


def _find_unit(text, candidates):
    lowered = text.lower()
    hits = [u for u in candidates if re.search(rf"(?<![a-z0-9]){re.escape(u)}(?![a-z0-9])", lowered)]
    if len(hits) > 1:
        hits = sorted(set("dbuv" if h in ("dbµv", "dbμv") else h for h in hits))
    return hits


def _find_unit_tiered(tiers, candidates, what, override_available):
    """Unit search from the most specific context outward: the column's own
    header cell, then the column-header line, then the analyzer preamble above
    it. The first tier that mentions any candidate decides — so "RBW 9 kHz" in
    the preamble can never contradict a column that states "Frequency (Hz)". A
    single hit is a STATED unit (beats any override, R-1); several distinct
    hits in one tier decide nothing — the override may resolve it, else raise.
    """
    for tier in tiers:
        hits = _find_unit(tier, candidates)
        if len(hits) == 1:
            return hits[0]
        if len(hits) > 1:
            if override_available:
                return None
            raise TraceFormatError(f"conflicting {what} units in header: {hits}")
    return None


@dataclass(frozen=True)
class _CsvScan:
    delimiter: str
    header_lines: tuple
    rows: tuple                   # EVERY numeric field per data row
    first_row_token_index: tuple  # token position of each numeric column
    numeric_columns: int          # max numeric fields over all rows


def _scan_csv(path):
    with open(path, encoding="utf-8-sig") as fh:
        lines = [line.rstrip("\n") for line in fh if line.strip()]
    if not lines:
        raise TraceFormatError(f"{path}: empty file")

    delimiter = _sniff_delimiter(lines[:20])
    header_lines = []
    rows = []
    first_positions = ()
    for line in lines:
        tokens = _split(line, delimiter)
        numeric, positions = [], []
        for position, tok in enumerate(tokens):
            value = _parse_field(tok, delimiter)
            if value is not None:
                numeric.append(value)
                positions.append(position)
        if len(numeric) >= 2:
            if not rows:
                first_positions = tuple(positions)
            rows.append(tuple(numeric))
        elif not rows:
            header_lines.append(line)

    if len(rows) < 2:
        raise TraceFormatError(f"{path}: fewer than two data rows found")
    return _CsvScan(delimiter, tuple(header_lines), tuple(rows), first_positions,
                    max(len(r) for r in rows))


def _column_header_cell(scan, column):
    """Header cell text for 1-based numeric column, or "" when absent. Cells
    align with the numeric columns through the first data row's token
    positions."""
    if not scan.header_lines or not 1 <= column <= len(scan.first_row_token_index):
        return ""
    cells = _split(scan.header_lines[-1], scan.delimiter)
    token_index = scan.first_row_token_index[column - 1]
    return cells[token_index] if token_index < len(cells) else ""


def _frequency_column(scan):
    """1-based numeric column carrying the frequency axis. Column 1 unless the
    header names exactly one OTHER column as frequency (a frequency unit token
    or "freq" in its cell) — the "No.,Frequency (MHz),Level" index-first
    export shape is common, and reading the index as megahertz fabricates the
    whole axis. Two frequency-looking columns are ambiguous and raise."""
    candidates = []
    for k in range(1, scan.numeric_columns + 1):
        cell = _column_header_cell(scan, k).lower()
        if not cell:
            continue
        unit_hit = any(
            re.search(rf"(?<![a-z0-9]){unit}(?![a-z0-9])", cell) for unit in _FREQ_UNITS
        )
        if unit_hit or "freq" in cell:
            candidates.append(k)
    if len(candidates) > 1:
        raise TraceFormatError(
            "several columns look like the frequency axis — clean the export so exactly "
            "one column names a frequency"
        )
    return candidates[0] if candidates else 1


def spectrum_csv_columns(path):
    """(count, frequency_column, names) of the numeric columns, for callers
    that must offer a level-column choice on multi-trace exports. names[k]
    labels 1-based numeric column k+1 ("" when the header has no cell for
    it)."""
    scan = _scan_csv(path)
    return scan.numeric_columns, _frequency_column(scan), [
        _column_header_cell(scan, k) for k in range(1, scan.numeric_columns + 1)
    ]


def read_spectrum_csv(path, freq_unit=None, level_unit=None, z0_ohm=50.0, level_column=None):
    """(frequencies_hz, levels_dbuv) from a frequency/level trace export.

    freq_unit: 'Hz'|'kHz'|'MHz'|'GHz', level_unit: 'dBuV'|'dBm' — only needed
    when the file header does not state them. level_column: 1-based numeric
    column holding the level — REQUIRED when the file carries more than two
    numeric columns (multi-trace export).
    """
    scan = _scan_csv(path)
    freq_column = _frequency_column(scan)

    if level_column is not None:
        level = int(level_column)
        if not 1 <= level <= scan.numeric_columns:
            raise TraceFormatError(
                f"{path}: level column {level} out of range — the file has "
                f"{scan.numeric_columns} numeric columns"
            )
        if level == freq_column:
            raise TraceFormatError(
                f"{path}: level column {level} is the frequency column — pick a level trace"
            )
    elif scan.numeric_columns == 2:
        level = 2 if freq_column == 1 else 1
    else:
        names = ", ".join(
            f'{k}: "{_column_header_cell(scan, k) or f"column {k}"}"'
            for k in range(1, scan.numeric_columns + 1)
            if k != freq_column
        )
        raise TraceFormatError(
            f"{path}: multiple level columns: the file carries {scan.numeric_columns} numeric "
            f"columns (a multi-trace export) — say which one to judge: {names}"
        )

    column_line = scan.header_lines[-1] if scan.header_lines else ""
    preamble = " ".join(scan.header_lines[:-1])
    file_freq_unit = _find_unit_tiered(
        (_column_header_cell(scan, freq_column), column_line, preamble),
        tuple(_FREQ_UNITS), "frequency", bool(freq_unit))
    file_level_unit = _find_unit_tiered(
        (_column_header_cell(scan, level), column_line, preamble),
        _LEVEL_UNITS, "level", bool(level_unit))
    # R-1 contract, enforced in the library and not only in GUIs: a unit the
    # header STATES always wins; the override only fills a silent/ambiguous axis.
    freq_unit = (file_freq_unit or freq_unit or "").lower()
    level_unit = (file_level_unit or level_unit or "").lower()
    if level_unit in ("dbµv", "dbμv"):
        level_unit = "dbuv"
    if freq_unit not in _FREQ_UNITS:
        raise TraceFormatError(
            f"{path}: frequency unit not stated in the file header — select it in the unit control"
        )
    if level_unit not in ("dbuv", "dbm"):
        raise TraceFormatError(
            f"{path}: level unit not stated in the file header — select it in the unit control"
        )

    needed = max(freq_column, level)
    for index, row in enumerate(scan.rows, start=1):
        if len(row) < needed:
            raise TraceFormatError(
                f"{path}: data row {index} has only {len(row)} numeric columns — needed {needed}"
            )
        f = row[freq_column - 1] * _FREQ_UNITS[freq_unit]
        if not math.isfinite(f) or not math.isfinite(row[level - 1]):
            raise TraceFormatError(
                f"{path}: non-finite value in data row {index} — clean the export before judging it"
            )
        if f <= 0.0:
            raise TraceFormatError(
                f"{path}: non-positive frequency ({f} Hz) in data row {index} — remove "
                "DC/index rows, and check the delimiter/decimal convention"
            )

    data = np.asarray([(row[freq_column - 1], row[level - 1]) for row in scan.rows], dtype=float)
    freqs = data[:, 0] * _FREQ_UNITS[freq_unit]
    levels = data[:, 1]
    if level_unit == "dbm":
        levels = dbuv_from_dbm(levels, z0_ohm=z0_ohm)
    order = np.argsort(freqs)
    return freqs[order], levels[order]
