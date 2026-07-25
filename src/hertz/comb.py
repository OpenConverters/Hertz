"""Harmonic-comb detection: estimate a converter's switching frequency from a
measured EMI spectrum and label which peaks its harmonics explain.

Method: peaks are local maxima with sufficient prominence over a log-frequency
median baseline; candidate fundamentals come from clustered adjacent-peak
spacings (plus the lowest peak itself); each candidate is scored by the summed
prominence of the peaks its harmonic grid matches, weighted by harmonic
coverage — the coverage factor is what stops a subharmonic (f_sw/2, which
matches everything f_sw matches) from winning. The best candidate is refined by
a prominence-weighted least-squares fit through the matched (n, f) pairs.

"No comb found" is a legitimate result, reported explicitly — never a guess.
"""

from dataclasses import dataclass, field

import numpy as np

PROMINENCE_THRESHOLD_DB = 8.0
BASELINE_HALF_WIDTH = 1.3  # median window: [f/1.3, f*1.3]
CLUSTER_TOLERANCE = 0.02
MIN_MATCHED_HARMONICS = 3
CONFIDENCE_THRESHOLD = 0.5
MAX_COVERAGE_HARMONICS = 20


@dataclass(frozen=True)
class MatchedHarmonic:
    order: int
    frequency_hz: float
    level_dbuv: float


@dataclass(frozen=True)
class CombResult:
    found: bool
    f_sw_hz: float | None = None
    confidence: float = 0.0
    coverage: float = 0.0
    harmonics: tuple = field(default_factory=tuple)
    residual_peaks: tuple = field(default_factory=tuple)  # (frequency_hz, level_dbuv)


def _baseline(freqs, levels):
    baseline = np.empty_like(levels)
    for i, f in enumerate(freqs):
        window = (freqs >= f / BASELINE_HALF_WIDTH) & (freqs <= f * BASELINE_HALF_WIDTH)
        baseline[i] = np.median(levels[window])
    return baseline


def _find_peaks(freqs, levels):
    prominence = levels - _baseline(freqs, levels)
    peaks = []
    for i in range(len(freqs)):
        lo = max(0, i - 2)
        hi = min(len(freqs), i + 3)
        if prominence[i] > PROMINENCE_THRESHOLD_DB and levels[i] >= levels[lo:hi].max():
            peaks.append(i)
    # collapse plateaus / near-duplicates (within 1 % in frequency): keep the strongest
    collapsed = []
    for i in peaks:
        if collapsed and freqs[i] < freqs[collapsed[-1]] * 1.01:
            if levels[i] > levels[collapsed[-1]]:
                collapsed[-1] = i
        else:
            collapsed.append(i)
    return collapsed, prominence


def _bin_spacing(freqs, f):
    index = int(np.clip(np.searchsorted(freqs, f), 1, len(freqs) - 1))
    return freqs[index] - freqs[index - 1]


def _match(freqs, peak_indices, prominence, candidate, f_max):
    matched = {}
    for i in peak_indices:
        f = freqs[i]
        order = int(round(f / candidate))
        if order < 1:
            continue
        tolerance = min(max(2.0 * _bin_spacing(freqs, f), 0.005 * f), 0.3 * candidate)
        if abs(f - order * candidate) <= tolerance:
            if order not in matched or prominence[i] > prominence[matched[order]]:
                matched[order] = i
    possible = max(1, min(int(f_max / candidate), MAX_COVERAGE_HARMONICS))
    coverage = sum(1 for order in matched if order <= possible) / possible
    score = sum(prominence[i] for i in matched.values())
    return matched, score * np.sqrt(coverage), coverage


def detect_comb(freqs_hz, levels_dbuv, f_sw_min_hz=20e3, f_sw_max_hz=5e6):
    freqs = np.asarray(freqs_hz, dtype=float)
    levels = np.asarray(levels_dbuv, dtype=float)
    if freqs.shape != levels.shape or freqs.ndim != 1:
        raise ValueError("frequency and level arrays must be one-dimensional and equal length")
    if freqs.size < 16:
        raise ValueError("spectrum too short for comb detection (need >= 16 points)")
    if np.any(np.diff(freqs) <= 0.0):
        raise ValueError("frequencies must be strictly increasing")
    if f_sw_min_hz <= 0.0 or f_sw_min_hz >= f_sw_max_hz:
        raise ValueError("bad f_sw search range")

    peak_indices, prominence = _find_peaks(freqs, levels)
    if len(peak_indices) < MIN_MATCHED_HARMONICS:
        return CombResult(found=False,
                          residual_peaks=tuple((freqs[i], levels[i]) for i in peak_indices))

    peak_freqs = freqs[peak_indices]
    candidates = []
    for gap in np.diff(peak_freqs):
        if f_sw_min_hz <= gap <= f_sw_max_hz:
            for cluster in candidates:
                if abs(gap - cluster[0] / cluster[1]) <= CLUSTER_TOLERANCE * gap:
                    cluster[0] += gap
                    cluster[1] += 1
                    break
            else:
                candidates.append([gap, 1])
    candidate_values = [total / count for total, count in candidates]
    if f_sw_min_hz <= peak_freqs[0] <= f_sw_max_hz:
        candidate_values.append(peak_freqs[0])
    if not candidate_values:
        return CombResult(found=False,
                          residual_peaks=tuple((freqs[i], levels[i]) for i in peak_indices))

    best = max((_match(freqs, peak_indices, prominence, c, freqs[-1]) + (c,)
                for c in candidate_values), key=lambda item: item[1])
    matched, _, _, candidate = best

    # prominence-weighted least squares through the origin: f ≈ n·f_sw
    weights = np.array([prominence[i] for i in matched.values()])
    orders = np.array(list(matched.keys()), dtype=float)
    matched_freqs = np.array([freqs[i] for i in matched.values()])
    f_sw = float(np.sum(weights * orders * matched_freqs) / np.sum(weights * orders**2))

    matched, _, coverage = _match(freqs, peak_indices, prominence, f_sw, freqs[-1])
    matched_set = set(matched.values())
    total_prominence = sum(prominence[i] for i in peak_indices)
    matched_prominence = sum(prominence[i] for i in matched_set)
    confidence = matched_prominence / total_prominence if total_prominence > 0.0 else 0.0

    found = confidence >= CONFIDENCE_THRESHOLD and len(matched) >= MIN_MATCHED_HARMONICS
    return CombResult(
        found=found,
        f_sw_hz=f_sw if found else None,
        confidence=confidence,
        coverage=coverage,
        harmonics=tuple(sorted(
            (MatchedHarmonic(order, float(freqs[i]), float(levels[i]))
             for order, i in matched.items()), key=lambda h: h.order)) if found else (),
        residual_peaks=tuple((float(freqs[i]), float(levels[i]))
                             for i in peak_indices if i not in matched_set),
    )
