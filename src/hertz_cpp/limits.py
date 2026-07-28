"""EMC emission limit lines — C++ engine (PyHertz).

Only shape adaptation lives here: the engine returns plain sequences, the
reference API returns numpy arrays shaped like the input.
"""

import numpy as np

from ._engine import engine

OutsideCoverage = engine.OutsideCoverage
LimitSegment = engine.LimitSegment
LimitLine = engine.LimitLine

CISPR32_CLASS_B_MAINS_QP = engine.CISPR32_CLASS_B_MAINS_QP
CISPR32_CLASS_B_MAINS_AVG = engine.CISPR32_CLASS_B_MAINS_AVG
CISPR32_CLASS_A_MAINS_QP = engine.CISPR32_CLASS_A_MAINS_QP
CISPR32_CLASS_A_MAINS_AVG = engine.CISPR32_CLASS_A_MAINS_AVG

UNSWEPT_GAP_DECADES = engine.UNSWEPT_GAP_DECADES
UNSWEPT_RELATIVE_FACTOR = engine.UNSWEPT_RELATIVE_FACTOR
UNSWEPT_LOCAL_WINDOW = engine.UNSWEPT_LOCAL_WINDOW
UNSWEPT_SEGMENT_SWALLOW = engine.UNSWEPT_SEGMENT_SWALLOW
UNSWEPT_MIN_HOLE_RBW = engine.UNSWEPT_MIN_HOLE_RBW

cispr25_conducted_voltage = engine.cispr25_conducted_voltage
cispr32_radiated = engine.cispr32_radiated
limit_polyline_runs = engine.limit_polyline_runs


def unswept_regions(line, f_lo_hz, f_hi_hz):
    return engine.unswept_regions(line, f_lo_hz, f_hi_hz)


def unswept_regions_sampled(line, freqs_hz):
    return engine.unswept_regions_sampled(line, [float(f) for f in freqs_hz])


# levels_where_covered is a METHOD in the reference API; the engine binds the
# per-point lookup and this wrapper only re-shapes it into numpy arrays.
if not hasattr(LimitLine, "_levels_where_covered_engine"):
    LimitLine._levels_where_covered_engine = LimitLine.levels_where_covered

    def _levels_where_covered(self, f_hz):
        array = np.asarray(f_hz, dtype=float)
        mask, levels = self._levels_where_covered_engine(array.ravel().tolist())
        return (np.asarray(mask, dtype=bool).reshape(array.shape),
                np.asarray(levels, dtype=float).reshape(array.shape))

    LimitLine.levels_where_covered = _levels_where_covered
