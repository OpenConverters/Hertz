"""Hertz with the C++ engine underneath — API-compatible with `hertz`.

Same names, same signatures, same exceptions as the pure-Python reference
package; every number comes from PyHertz (cpp/include/hertz/*.hpp). The modules
here only adapt shapes (numpy arrays in/out, tuple returns, path-taking CSV
readers) — no formula is reimplemented, so pointing the reference test suite at
this package (pytest --backend=cpp) compares the two engines directly.
"""

from .comb import detect_comb
from .detector import (
    BAND_A,
    BAND_B,
    BAND_C,
    BAND_D,
    CisprBand,
    band_for_frequency,
    measure,
    stft_envelope,
)
from .filter_design import (
    LineFilterDesign,
    achieved_attenuation_db,
    cutoff_frequency,
    design_frequency,
    design_line_filter,
    discharge_resistor_power,
    leakage_inductance_from_impedance,
    max_discharge_resistance,
    required_attenuation_db,
    y_capacitor_leakage_current,
)
from .limits import (
    CISPR32_CLASS_A_MAINS_AVG,
    CISPR32_CLASS_A_MAINS_QP,
    CISPR32_CLASS_B_MAINS_AVG,
    CISPR32_CLASS_B_MAINS_QP,
    LimitLine,
    LimitSegment,
    OutsideCoverage,
    cispr25_conducted_voltage,
)
from .lisn import Lisn, cispr16_lisn, cispr25_lisn
from .separation import separate
from .traces import TraceFormatError, read_spectrum_csv
from .utils import E6, E12, E24, dbuv_from_dbm, dbuv_from_vrms, round_up_to, vrms_from_dbuv

__version__ = "0.1.0"
