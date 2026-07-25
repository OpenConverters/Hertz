"""EMC emission limit lines.

Limit *values* are facts of the referenced standards; the standard texts are not
reproduced here. Levels are dBµV, interpolated linearly against log10(f) inside
each segment (the shape CISPR limits are defined with). Frequencies outside every
segment raise OutsideCoverage: CISPR 25 defines limits only inside protected
broadcast bands, so "no limit here" is real information, never a zero.
"""

from dataclasses import dataclass

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
        for s in self.segments:
            if s.f_start_hz <= f_hz <= s.f_stop_hz:
                return float(s.level(f_hz))
        raise OutsideCoverage(f"{f_hz} Hz is outside limit line {self.name!r}")

    def levels_where_covered(self, f_hz):
        """(mask, levels) arrays; levels is NaN where the line does not apply."""
        f = np.asarray(f_hz, dtype=float)
        mask = np.zeros(f.shape, dtype=bool)
        levels = np.full(f.shape, np.nan)
        for s in self.segments:
            inside = (f >= s.f_start_hz) & (f <= s.f_stop_hz)
            mask |= inside
            levels[inside] = s.level(f[inside])
        return mask, levels

    def margin(self, f_hz, measured_dbuv):
        """limit - measured: positive means passing."""
        return self.level(f_hz) - measured_dbuv


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
_CISPR25_BANDS_CLASS5_QP = (
    ("LW", 150e3, 300e3, 57.0),
    ("MW", 530e3, 1.8e6, 41.0),
    ("SW", 5.9e6, 6.2e6, 40.0),
    ("CB", 26e6, 28e6, 31.0),
    ("FM", 76e6, 108e6, 25.0),
)
_CISPR25_DETECTOR_OFFSET = {"peak": 13.0, "quasi_peak": 0.0, "average": -7.0}


def cispr25_conducted_voltage(emission_class, detector):
    """CISPR 25 conducted-voltage limit line for class 1-5 and a given detector."""
    if emission_class not in (1, 2, 3, 4, 5):
        raise ValueError("CISPR 25 class must be 1..5")
    offset = _CISPR25_DETECTOR_OFFSET[detector] + 10.0 * (5 - emission_class)
    segments = [
        LimitSegment(f0, f1, qp5 + offset, qp5 + offset)
        for _, f0, f1, qp5 in _CISPR25_BANDS_CLASS5_QP
    ]
    return LimitLine(f"CISPR 25 Class {emission_class} conducted ({detector})", detector, segments)
