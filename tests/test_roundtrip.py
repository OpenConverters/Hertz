"""ABT #299: the ngspice round-trip cross-check.

The SAME deck SpiceDeck.hpp emits for the app (via cpp/build/deck_dump — one
emitter, zero drift) is solved by Kirchhoff's IN-PROCESS libngspice
(PyKirchhoff.run_ngspice_ac; never the installed ngspice binary, per the house
rule), and the resulting insertion loss is compared against the analytic ABCD
twin evaluated under the deck's ACTUAL terminations.

IL derivation: the filter deck and a reference deck (same drive, same LISNs,
no filter) are both run; IL(f) = vdb_ref(meas) − vdb_filt(meas). The LISN's
own eut→receiver divider cancels exactly in the ratio.

Tolerance: 0.5 dB across 150 kHz–30 MHz. Both sides use identical ideal
elements (the deck states ESL/ESR are not modeled), the identical LISN network
(eut_impedance IS the subckt), the exact common-mode inductance of the equal-K
coupled stack L·(1+(n−1)K)/n, and the DM loop's Y-pair series capacitance —
the remaining difference is ngspice numerics. A disagreement beyond 0.5 dB
means one of the two engines models a DIFFERENT circuit, which is precisely
what this test exists to catch.
"""
import cmath
import math
import subprocess
import sys
from pathlib import Path

import pytest

from hertz.lisn import cispr16_lisn
from hertz.network import cascade, insertion_loss_db, series_impedance, shunt_admittance, Abcd

REPO = Path(__file__).resolve().parents[1]
DECK_DUMP = REPO / "cpp" / "build" / "deck_dump"
KH_BUILD = Path.home() / "OpenConverters" / "Kirchhoff" / "build"

PyKirchhoff = None
if any(KH_BUILD.glob("PyKirchhoff*.so")):
    sys.path.insert(0, str(KH_BUILD))
    try:
        import PyKirchhoff
    except ImportError:
        PyKirchhoff = None

pytestmark = pytest.mark.skipif(
    PyKirchhoff is None or not DECK_DUMP.exists(),
    reason="needs cpp/build/deck_dump and a PyKirchhoff build with libngspice",
)


def _deck(which, mode, n_lines, stages, l_cm, c_x, c_y, l_dm):
    out = subprocess.run(
        [str(DECK_DUMP), which, mode, str(n_lines), str(stages),
         repr(l_cm), repr(c_x), repr(c_y), repr(l_dm), "cispr16"],
        capture_output=True, text=True, check=True)
    return out.stdout


def _meas_vector(result, port):
    key = next(k for k in result["vectors"] if k.lower() == port)
    re = result["vectors"][key]["re"]
    im = result["vectors"][key]["im"]
    return [math.hypot(a, b) for a, b in zip(re, im)]


def _spice_il(mode, n_lines, stages, l_cm, c_x, c_y, l_dm):
    port = "meas_line" if n_lines == 2 else "meas_l1"
    filt = PyKirchhoff.run_ngspice_ac(_deck("filter", mode, n_lines, stages, l_cm, c_x, c_y, l_dm))
    ref = PyKirchhoff.run_ngspice_ac(_deck("reference", mode, n_lines, stages, l_cm, c_x, c_y, l_dm))
    assert filt["success"], filt["error"]
    assert ref["success"], ref["error"]
    f_filt, f_ref = filt["frequenciesHz"], ref["frequenciesHz"]
    assert len(f_filt) == len(f_ref) and f_filt[0] == f_ref[0] and f_filt[-1] == f_ref[-1]
    v_filt = _meas_vector(filt, port)
    v_ref = _meas_vector(ref, port)
    il = [20.0 * math.log10(r / x) for r, x in zip(v_ref, v_filt)]
    return f_filt, il


def _twin_il(freqs, mode, n_lines, stages, l_cm, c_x, c_y, l_dm, x_factor):
    lisn = cispr16_lisn()
    k = 1.0 - l_dm / (2.0 * l_cm)
    assert 0.0 < k < 1.0
    out = []
    for f in freqs:
        w = 2.0 * math.pi * f
        z_lisn = lisn.eut_impedance(f)
        if mode == "cm":
            l_eff = l_cm * (1.0 + (n_lines - 1) * k) / n_lines
            series_l, shunt_c = l_eff, n_lines * c_y
            z_source, z_load = 1e-3 / n_lines, z_lisn / n_lines
        else:
            series_l, shunt_c = l_dm, c_x * x_factor + c_y / 2.0
            z_source, z_load = 0.0, 2.0 * z_lisn
        stage = cascade(series_impedance(1j * w * series_l),
                        shunt_admittance(1j * w * shunt_c))
        network = Abcd()
        for _ in range(stages):
            network = cascade(network, stage)
        out.append(insertion_loss_db(network, z_source, z_load))
    return out


@pytest.mark.parametrize("mode", ["cm", "dm"])
def test_mains_deck_agrees_with_the_abcd_twin(mode):
    args = dict(n_lines=2, stages=1, l_cm=1e-3, c_x=470e-9, c_y=4.7e-9, l_dm=14.6e-6)
    freqs, spice = _spice_il(mode, **args)
    twin = _twin_il(freqs, mode, x_factor=1.0, **args)
    worst = max(abs(a - b) for a, b in zip(spice, twin))
    assert worst < 0.5, f"{mode}: engines disagree by {worst:.2f} dB"


@pytest.mark.parametrize("mode", ["cm", "dm"])
def test_three_phase_deck_loads_and_agrees(mode):
    # all-pairs K on a 3-winding stack must load without a singular matrix, and
    # the delta-X effective capacitance (1.5 C per line pair) must hold
    args = dict(n_lines=3, stages=1, l_cm=1e-3, c_x=680e-9, c_y=33e-9, l_dm=14.6e-6)
    freqs, spice = _spice_il(mode, **args)
    twin = _twin_il(freqs, mode, x_factor=1.5 if mode == "dm" else 1.0, **args)
    worst = max(abs(a - b) for a, b in zip(spice, twin))
    assert worst < 0.5, f"3ph {mode}: engines disagree by {worst:.2f} dB"


def test_loose_coupling_cm_needs_the_k_aware_twin():
    # leakage-floored designs have K near zero: the coupled stack's CM
    # inductance is ~L/2, NOT L — the plain-L twin would be ~5 dB off, and this
    # cross-check is what catches such modeling drift
    args = dict(n_lines=2, stages=1, l_cm=8e-6, c_x=470e-9, c_y=33e-9, l_dm=14.6e-6)
    freqs, spice = _spice_il("cm", **args)
    twin = _twin_il(freqs, "cm", x_factor=1.0, **args)
    worst = max(abs(a - b) for a, b in zip(spice, twin))
    assert worst < 0.5, f"loose-K cm: engines disagree by {worst:.2f} dB"
