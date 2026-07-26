"""EMC emission limit lines.

Limit *values* are facts of the referenced standards; the standard texts are not
reproduced here. Levels are dBµV, interpolated linearly against log10(f) inside
each segment (the shape CISPR limits are defined with). Frequencies outside every
segment raise OutsideCoverage: CISPR 25 defines limits only inside protected
broadcast bands, so "no limit here" is real information, never a zero.
"""

from dataclasses import dataclass

import math

import numpy as np


class OutsideCoverage(ValueError):
    """Requested frequency is outside every segment of the limit line."""


@dataclass(frozen=True)
class LimitSegment:
    f_start_hz: float
    f_stop_hz: float
    level_start_dbuv: float
    level_stop_dbuv: float

    def __post_init__(self):
        if not 0.0 < self.f_start_hz < self.f_stop_hz:
            raise ValueError(f"invalid segment frequencies [{self.f_start_hz}, {self.f_stop_hz}]")

    def level(self, f_hz):
        span = np.log10(self.f_stop_hz) - np.log10(self.f_start_hz)
        frac = (np.log10(f_hz) - np.log10(self.f_start_hz)) / span
        return self.level_start_dbuv + frac * (self.level_stop_dbuv - self.level_start_dbuv)


class LimitLine:
    def __init__(self, name, detector, segments):
        if detector not in ("peak", "quasi_peak", "average"):
            raise ValueError(f"unknown detector {detector!r}")
        if not segments:
            raise ValueError("limit line needs at least one segment")
        ordered = sorted(segments, key=lambda s: s.f_start_hz)
        for a, b in zip(ordered, ordered[1:]):
            if b.f_start_hz < a.f_stop_hz * (1.0 - 1e-9):
                raise ValueError(f"overlapping segments in limit line {name!r}")
        self.name = name
        self.detector = detector
        self.segments = tuple(ordered)

    def covers(self, f_hz):
        return any(s.f_start_hz <= f_hz <= s.f_stop_hz for s in self.segments)

    def level(self, f_hz):
        # At a shared segment boundary the LOWER limit applies (the CISPR
        # transition rule) — never "whichever segment happens to come first".
        # Class A at exactly 500 kHz is 73 dBuV, not 79.
        covering = [float(s.level(f_hz)) for s in self.segments
                    if s.f_start_hz <= f_hz <= s.f_stop_hz]
        if not covering:
            raise OutsideCoverage(f"{f_hz} Hz is outside limit line {self.name!r}")
        return min(covering)

    def levels_where_covered(self, f_hz):
        """(mask, levels) arrays; levels is NaN where the line does not apply."""
        f = np.asarray(f_hz, dtype=float)
        mask = np.zeros(f.shape, dtype=bool)
        levels = np.full(f.shape, np.nan)
        for s in self.segments:
            inside = (f >= s.f_start_hz) & (f <= s.f_stop_hz)
            mask |= inside
            # boundary points covered by two segments take the LOWER limit
            levels[inside] = np.where(
                np.isnan(levels[inside]), s.level(f[inside]),
                np.minimum(levels[inside], s.level(f[inside])))
        return mask, levels

    def margin(self, f_hz, measured_dbuv):
        """limit - measured: positive means passing."""
        return self.level(f_hz) - measured_dbuv


# A consecutive-sample gap wider than this (in decades; 0.35 dec = a factor
# 2.24 in frequency) is a HOLE in the sweep, not sampling density: any real
# analyzer export sits far below it, and a "scan" that jumps a factor > 2 has
# simply not measured the spectrum in between.
UNSWEPT_GAP_DECADES = 0.35
# ...OR grossly out of line with the scan's OWN spacing in BOTH domains:
# > 6x the 90th-percentile gap in log-frequency (floored at 0.05 dec) AND in
# linear frequency. A log sweep is uniform in log, a linear export is uniform
# in Hz — a gap that is an outlier against both is a jump, not density. A
# ratio alone let a 16 MHz hole at 2.23x pass silently (round 13).
UNSWEPT_RELATIVE_FACTOR = 6.0
UNSWEPT_RELATIVE_FLOOR_DECADES = 0.05


def _cispr_rbw_hz(f_hz):
    """CISPR 16-1-1 resolution bandwidth of the band containing f — an
    unswept region narrower than the receiver's own RBW cannot even be
    resolved as unmeasured (it also swallows the float dust and
    half-RBW-below-stop slivers ordinary exports produce at band edges)."""
    if f_hz < 150e3:
        return 200.0
    if f_hz < 30e6:
        return 9e3
    return 120e3


def _merge_regions(regions):
    regions = sorted(regions)
    merged = []
    for f0, f1 in regions:
        if merged and f0 <= merged[-1][1] * (1.0 + 1e-9):
            merged[-1][1] = max(merged[-1][1], f1)
        else:
            merged.append([f0, f1])
    return [tuple(r) for r in merged]


def unswept_regions(line, f_lo_hz, f_hi_hz):
    """Parts of a limit line's regulated spans the scan NEVER REACHED —
    everything of each segment outside [f_lo_hz, f_hi_hz] (the scan's own
    extent). A verdict naming a standard implies the standard's whole range;
    contiguous regions across touching segments are merged for reporting."""
    regions = []
    for s in line.segments:
        if s.f_start_hz < f_lo_hz:
            regions.append((s.f_start_hz, min(s.f_stop_hz, f_lo_hz)))
        if s.f_stop_hz > f_hi_hz:
            regions.append((max(s.f_start_hz, f_hi_hz), s.f_stop_hz))
    return [r for r in _merge_regions(regions) if r[1] - r[0] >= _cispr_rbw_hz(r[0])]


def unswept_regions_sampled(line, freqs_hz):
    """Sampling-aware variant: extent regions PLUS interior holes — regulated
    sub-spans the sweep jumped over (spliced band-by-band acquisitions, a lost
    segment, a retune gap). Reported holes are clipped to the limit's
    segments, so the unregulated space between CISPR 25 bands is never
    flagged."""
    freqs = sorted(float(f) for f in freqs_hz)
    if not freqs:
        raise ValueError("unswept_regions_sampled needs at least one sample frequency")
    regions = list(unswept_regions(line, freqs[0], freqs[-1]))
    def _p90(values):
        values = sorted(values)
        return values[min(len(values) - 1, (len(values) * 9) // 10)] if values else 0.0

    log_p90 = _p90([math.log10(fb / fa) for fa, fb in zip(freqs, freqs[1:]) if fa > 0.0])
    lin_p90 = _p90([fb - fa for fa, fb in zip(freqs, freqs[1:]) if fa > 0.0])
    for fa, fb in zip(freqs, freqs[1:]):
        if fa <= 0.0:
            continue
        gap = math.log10(fb / fa)
        hole = gap > UNSWEPT_GAP_DECADES or (
            gap > UNSWEPT_RELATIVE_FLOOR_DECADES
            and gap > UNSWEPT_RELATIVE_FACTOR * log_p90
            and fb - fa > UNSWEPT_RELATIVE_FACTOR * lin_p90)
        if not hole:
            continue
        for s in line.segments:
            f0, f1 = max(fa, s.f_start_hz), min(fb, s.f_stop_hz)
            if f0 < f1:
                regions.append((f0, f1))
    # A regulated segment the scan's extent overlaps but that contains NO
    # sample at all is unswept regardless of any gap ratio — CISPR 25's
    # narrow bands fit whole inside a modest-looking jump (25 -> 30 MHz
    # skips 26-28 MHz entirely at a factor of only 1.2).
    for s in line.segments:
        f0, f1 = max(s.f_start_hz, freqs[0]), min(s.f_stop_hz, freqs[-1])
        if f0 < f1 and not any(s.f_start_hz <= f <= s.f_stop_hz for f in freqs):
            regions.append((f0, f1))
    return [r for r in _merge_regions(regions) if r[1] - r[0] >= _cispr_rbw_hz(r[0])]


# CISPR 32 / EN 55032, AC mains conducted, 150 kHz - 30 MHz.
CISPR32_CLASS_B_MAINS_QP = LimitLine("CISPR 32 Class B mains (QP)", "quasi_peak", [
    LimitSegment(150e3, 500e3, 66.0, 56.0),
    LimitSegment(500e3, 5e6, 56.0, 56.0),
    LimitSegment(5e6, 30e6, 60.0, 60.0),
])
CISPR32_CLASS_B_MAINS_AVG = LimitLine("CISPR 32 Class B mains (AVG)", "average", [
    LimitSegment(150e3, 500e3, 56.0, 46.0),
    LimitSegment(500e3, 5e6, 46.0, 46.0),
    LimitSegment(5e6, 30e6, 50.0, 50.0),
])
CISPR32_CLASS_A_MAINS_QP = LimitLine("CISPR 32 Class A mains (QP)", "quasi_peak", [
    LimitSegment(150e3, 500e3, 79.0, 79.0),
    LimitSegment(500e3, 30e6, 73.0, 73.0),
])
CISPR32_CLASS_A_MAINS_AVG = LimitLine("CISPR 32 Class A mains (AVG)", "average", [
    LimitSegment(150e3, 500e3, 66.0, 66.0),
    LimitSegment(500e3, 30e6, 60.0, 60.0),
])

# CISPR 25 conducted emissions, voltage method, supply lines: limits exist only in
# the protected broadcast bands. Class 5 QP baseline per CISPR 25:2016 Table 5;
# peak = QP + 13 dB, average = QP - 7 dB, and each class step relaxes 10 dB.
# CISPR 25 Table 4, conducted voltage method: (name, f0, f1, class-5 QP, class
# step). The class-to-class step is NOT a flat 10 dB: 10 dB in LW, 8 dB in MW,
# 6 dB from SW up — deriving class 3/4 as class 5 + 10/20 dB was up to 8 dB too
# permissive. The 68-87 MHz mobile-services band and the 76-108 MHz FM band
# carry identical limits at every class and detector, so their union is one
# segment.
_CISPR25_BANDS_CLASS5_QP = (
    ("LW", 150e3, 300e3, 57.0, 10.0),
    ("MW", 530e3, 1.8e6, 41.0, 8.0),
    ("SW", 5.9e6, 6.2e6, 40.0, 6.0),
    ("CB", 26e6, 28e6, 31.0, 6.0),
    ("VHF 30-54", 30e6, 54e6, 31.0, 6.0),
    ("VHF/FM 68-108", 68e6, 108e6, 25.0, 6.0),
)
_CISPR25_DETECTOR_OFFSET = {"peak": 13.0, "quasi_peak": 0.0, "average": -7.0}


def cispr25_conducted_voltage(emission_class, detector):
    """CISPR 25 conducted-voltage limit line for class 1-5 and a given detector."""
    if emission_class not in (1, 2, 3, 4, 5):
        raise ValueError("CISPR 25 class must be 1..5")
    detector_offset = _CISPR25_DETECTOR_OFFSET[detector]
    segments = [
        LimitSegment(f0, f1, qp5 + detector_offset + step * (5 - emission_class),
                     qp5 + detector_offset + step * (5 - emission_class))
        for _, f0, f1, qp5, step in _CISPR25_BANDS_CLASS5_QP
    ]
    return LimitLine(f"CISPR 25 Class {emission_class} conducted ({detector})", detector, segments)
