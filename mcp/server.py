"""Hertz MCP App server — conducted-EMI pre-compliance inside an LLM chat.

Exposes the Hertz C++ engine (``cpp/include/hertz/``, through the ``PyHertz``
pybind11 module) as MCP tools, and ships an interactive spectrum-vs-limit widget
as an MCP Apps UI resource (SEP-1865). Hosts that understand MCP Apps render the
widget; hosts that only speak plain MCP still get the tools and the text verdict.

Run:
    python mcp/server.py                 # streamable HTTP on 127.0.0.1:8400/mcp
    HERTZ_ENGINE=python python mcp/server.py   # numpy reference instead
"""

from __future__ import annotations

import importlib
import json
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

_REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO / "src"))

# --- engine -----------------------------------------------------------------
# The C++ core is the product engine and the default here; the numpy reference
# stays reachable with HERTZ_ENGINE=python for A/B work. That is an explicit
# choice, never a fallback: an unbuilt PyHertz raises with build instructions
# rather than quietly serving different numbers than the web app does. The two
# packages are API-identical, so everything below reads the same either way.
_ENGINE = os.environ.get("HERTZ_ENGINE", "cpp").strip().lower()
if _ENGINE not in ("cpp", "python"):
    raise ValueError(f"HERTZ_ENGINE must be 'cpp' or 'python' -- got {_ENGINE!r}")
_PKG = "hertz_cpp" if _ENGINE == "cpp" else "hertz"

comb = importlib.import_module(f"{_PKG}.comb")
detector = importlib.import_module(f"{_PKG}.detector")
filter_design = importlib.import_module(f"{_PKG}.filter_design")
limits = importlib.import_module(f"{_PKG}.limits")
lisn_models = importlib.import_module(f"{_PKG}.lisn")
network = importlib.import_module(f"{_PKG}.network")
radiated = importlib.import_module(f"{_PKG}.radiated")
separation = importlib.import_module(f"{_PKG}.separation")
traces = importlib.import_module(f"{_PKG}.traces")
utils = importlib.import_module(f"{_PKG}.utils")

detect_comb = comb.detect_comb
band_for_frequency = detector.band_for_frequency
measure = detector.measure
achieved_attenuation_db = filter_design.achieved_attenuation_db
cutoff_frequency = filter_design.cutoff_frequency
design_frequency = filter_design.design_frequency
design_line_filter = filter_design.design_line_filter
discharge_resistor_power = filter_design.discharge_resistor_power
input_filter_interaction = filter_design.input_filter_interaction
max_discharge_resistance = filter_design.max_discharge_resistance
required_attenuation_db = filter_design.required_attenuation_db
y_capacitor_leakage_current = filter_design.y_capacitor_leakage_current
CISPR32_CLASS_A_MAINS_AVG = limits.CISPR32_CLASS_A_MAINS_AVG
CISPR32_CLASS_A_MAINS_QP = limits.CISPR32_CLASS_A_MAINS_QP
CISPR32_CLASS_B_MAINS_AVG = limits.CISPR32_CLASS_B_MAINS_AVG
CISPR32_CLASS_B_MAINS_QP = limits.CISPR32_CLASS_B_MAINS_QP
cispr25_conducted_voltage = limits.cispr25_conducted_voltage
cispr32_radiated = limits.cispr32_radiated
unswept_regions_sampled = limits.unswept_regions_sampled
cispr16_lisn = lisn_models.cispr16_lisn
cispr25_lisn = lisn_models.cispr25_lisn
radiated_efield_dbuvm = radiated.radiated_efield_dbuvm
separate = separation.separate
read_spectrum_csv = traces.read_spectrum_csv
spectrum_csv_columns = traces.spectrum_csv_columns

from mcp.server.fastmcp import FastMCP                 # noqa: E402
from mcp.server.transport_security import (            # noqa: E402
    TransportSecuritySettings,
)
from mcp.types import CallToolResult, TextContent      # noqa: E402

# --- MCP Apps wire constants (from @modelcontextprotocol/ext-apps 1.7.5) -----
UI_RESOURCE_MIME = "text/html;profile=mcp-app"
UI_RESOURCE_URI = "ui://hertz/spectrum.html"     # scan vs limit, clickable
UI_CURVES_URI = "ui://hertz/curves.html"         # standalone fallback chart
UI_CHART_URI = "ui://hertz/chart.html"           # the web app's own LogChart
UI_FILTER_URI = "ui://hertz/filter.html"         # FilterSchematic + LogChart
UI_WASMPROBE_URI = "ui://hertz/wasmprobe.html"   # CSP capability probe


def _ui_meta(uri: str) -> dict:
    """registerAppTool() emits BOTH the flat key and the nested object, so
    hosts reading either form find it. Mirror that exactly."""
    return {"ui/resourceUri": uri, "ui": {"resourceUri": uri}}


UI_META = _ui_meta(UI_RESOURCE_URI)
# Curve-producing tools render through the web app's LogChart, so a chart in
# Claude is the same component as a chart on hertz.openconverters.com.
UI_CURVES_META = _ui_meta(UI_CHART_URI)
UI_FILTER_META = _ui_meta(UI_FILTER_URI)
UI_WASMPROBE_META = _ui_meta(UI_WASMPROBE_URI)


def _eng(value: float, unit: str) -> str:
    """Engineering-notation label, e.g. 3.3 mH / 4.7 nF."""
    for factor, prefix in ((1e-12, "p"), (1e-9, "n"), (1e-6, "µ"),
                           (1e-3, "m"), (1.0, "")):
        if abs(value) < factor * 1000.0:
            return f"{value / factor:.3g} {prefix}{unit}"
    return f"{value:.3g} {unit}"

# A chart has ~1000 useful pixels across; a 30k-point scan does not need to
# cross the wire. Decimation is PEAK-PRESERVING (max per log-frequency bin) --
# averaging or strided sampling would hide the very offender the engineer is
# looking for.
MAX_TRACE_POINTS = 900

# The SDK's DNS-rebinding protection rejects any Host header it does not
# recognise with a bare "421 Invalid Host header". Behind a tunnel or reverse
# proxy the Host is the PUBLIC name, not localhost -- so every request dies at
# 421, and a remote host that cannot speak MCP typically falls back to probing
# for OAuth, surfacing as a misleading "couldn't register with the sign-in
# service" error. Name the public host here (HERTZ_PUBLIC_HOST), or set
# HERTZ_ALLOW_ANY_HOST=1 for throwaway tunnels whose name changes per run.
_public_host = os.environ.get("HERTZ_PUBLIC_HOST", "").strip()
# Accept a pasted URL, not just a bare hostname: a Host header carries neither
# scheme nor path, so "https://x.trycloudflare.com/mcp" would never match the
# incoming "x.trycloudflare.com" and the 421 masquerades as an OAuth failure.
if "://" in _public_host:
    _public_host = _public_host.split("://", 1)[1]
_public_host = _public_host.split("/", 1)[0].strip()
if os.environ.get("HERTZ_ALLOW_ANY_HOST") == "1":
    _security = TransportSecuritySettings(enable_dns_rebinding_protection=False)
else:
    _allowed = ["127.0.0.1:8400", "localhost:8400", "127.0.0.1", "localhost"]
    if _public_host:
        _allowed += [_public_host, f"{_public_host}:443"]
    # allowed_origins is matched EXACTLY (or with a trailing ":*" port wildcard)
    # — a bare "*" is not a wildcard, just a literal that never matches, so it
    # reads as "allow everything" while 403-ing every browser-resident host.
    # Name the origins that actually call: Claude, a local reference host, and
    # the tunnel itself. HERTZ_ALLOWED_ORIGINS adds more, comma-separated.
    _origins = ["https://claude.ai", "https://www.claude.ai",
                "http://localhost:*", "http://127.0.0.1:*"]
    if _public_host:
        _origins.append(f"https://{_public_host}")
    _origins += [o.strip() for o in
                 os.environ.get("HERTZ_ALLOWED_ORIGINS", "").split(",") if o.strip()]
    _security = TransportSecuritySettings(allowed_hosts=_allowed, allowed_origins=_origins)

mcp = FastMCP("Hertz EMC", host="127.0.0.1", port=8400, transport_security=_security)


# --- helpers ----------------------------------------------------------------

def _with_csv(csv_text: str, fn):
    """Run `fn(path)` over CSV text. Hertz's readers take paths (the C++ core
    is string-based; the Python reference is not), so the text is spilled to a
    temp file and removed immediately."""
    with tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False) as fh:
        fh.write(csv_text)
        path = fh.name
    try:
        return fn(path)
    finally:
        os.unlink(path)


def _numeric_csv(csv_text: str, n_columns: int, what: str):
    """Parse a plain numeric CSV with a header line into `n_columns` arrays.

    Used by the tools whose input is not a frequency/level trace (waveforms,
    complex line pairs), where Hertz's unit-sniffing reader does not apply.
    """
    rows = []
    for raw in csv_text.splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = [p.strip() for p in line.replace(";", ",").replace("\t", ",").split(",")]
        try:
            vals = [float(p) for p in parts[:n_columns]]
        except ValueError:
            continue  # header or comment
        if len(vals) == n_columns:
            rows.append(vals)
    if len(rows) < 2:
        raise ValueError(
            f"could not read {n_columns} numeric columns ({what}) from the CSV -- "
            f"found {len(rows)} usable row(s). Expected: {what}."
        )
    arr = np.asarray(rows, dtype=float)
    return [arr[:, i] for i in range(n_columns)]


def _curves_result(title, subtitle, y_label, y_unit, series, summary,
                   markers=None, note=None):
    """Build a tool result for the generic curves widget.

    Same two-channel discipline as check_spectrum: `content` is the digest the
    model reads, `structuredContent` is the payload only the widget reads.
    """
    return CallToolResult(
        content=[TextContent(type="text", text=summary)],
        structuredContent={
            "title": title,
            "subtitle": subtitle,
            "y_label": y_label,
            "y_unit": y_unit,
            "series": series,
            "markers": markers or [],
            "note": note,
        },
    )


def _sample_log(f_lo: float, f_hi: float, per_decade: int = 60):
    decades = max(np.log10(f_hi / f_lo), 1e-9)
    return np.logspace(np.log10(f_lo), np.log10(f_hi),
                       max(16, int(decades * per_decade)))


def _limit_series(line, name=None):
    """Limit line as widget series, one polyline per segment.

    Segments are joined by a NaN break so the widget never draws a limit
    through a band the standard leaves unregulated.
    """
    pts = []
    for seg in line.segments:
        pts.append([seg.f_start_hz, seg.level_start_dbuv])
        pts.append([seg.f_stop_hz, seg.level_stop_dbuv])
        pts.append([seg.f_stop_hz, float("nan")])   # break
    return {"name": name or line.name, "points": pts, "style": "dashed",
            "color": "var(--s-limit)"}


_KELVIN = _REPO / "web" / "public" / "kelvin"


def _catalog(kind: str) -> list[dict]:
    """Parts for a component kind, from the Kelvin exports the web app serves.

    Raises loudly when the catalog is absent: a filter designed against an
    empty parts list would silently look like "no part fits" when the truth is
    "nobody looked". Regenerate with scripts/export_cmc_catalog.py /
    scripts/export_safety_caps.py.
    """
    fname = "hertz-cmc.v1.json" if kind == "cmc" else "hertz-safety-caps.v1.json"
    path = _KELVIN / fname
    if not path.exists():
        raise FileNotFoundError(
            f"{path} missing -- no {kind} catalog to select from. Regenerate it "
            f"with scripts/export_{'cmc_catalog' if kind == 'cmc' else 'safety_caps'}.py."
        )
    return json.loads(path.read_text(encoding="utf-8")).get("parts", [])


def _candidate_rows(kind: str, target: float, tolerance_pct: float,
                    manufacturer: str | None, min_current_a: float,
                    min_voltage_v: float, limit: int) -> list[dict]:
    """Catalog parts near `target`, ranked by how close they sit to it."""
    lo = target * (1.0 - tolerance_pct / 100.0)
    hi = target * (1.0 + tolerance_pct / 100.0)
    rows = []
    for p in _catalog("cmc" if kind == "cmc" else "cap"):
        if manufacturer and manufacturer.lower() not in (p.get("manufacturer") or "").lower():
            continue
        if kind == "cmc":
            value = p.get("inductanceH")
            if value is None or not lo <= value <= hi:
                continue
            if min_current_a and (p.get("ratedCurrentA") or 0.0) < min_current_a:
                continue
            row = {
                "mpn": p["mpn"], "manufacturer": p.get("manufacturer"),
                "family": p.get("family"), "value_h": value,
                "value": _eng(value, "H"),
                "rated_current_a": p.get("ratedCurrentA"),
                "dcr_ohm": p.get("dcrOhm"),
                "windings": p.get("windings"),
                "has_measured_curve": bool(p.get("hasMeasuredCmCurve")),
            }
        else:
            value = p.get("capacitanceF")
            if value is None or not lo <= value <= hi:
                continue
            klass = (p.get("safetyClass") or "").upper()
            # X capacitors go line-to-line, Y line-to-earth. Offering an X2 for
            # a Y position is a safety error, not a preference.
            want = "X" if kind == "cx" else "Y"
            if klass and not klass.startswith(want):
                continue
            if min_voltage_v and (p.get("ratedVoltageV") or 0.0) < min_voltage_v:
                continue
            row = {
                "mpn": p["mpn"], "manufacturer": p.get("manufacturer"),
                "family": p.get("family"), "value_f": value,
                "value": _eng(value, "F"),
                "safety_class": p.get("safetyClass"),
                "rated_voltage_v": p.get("ratedVoltageV"),
                "technology": p.get("technology"),
            }
        row["deviation_pct"] = (value - target) / target * 100.0
        rows.append(row)
    rows.sort(key=lambda r: abs(r["deviation_pct"]))
    return rows[:limit]


def _lisn_series(model: str) -> dict:
    """LISN impedance as a widget payload (name + curves)."""
    lisn = _lisn(model)
    freqs = _sample_log(9e3, 30e6)
    eut = [float(abs(lisn.eut_impedance(f))) for f in freqs]
    return {
        "name": lisn.name,
        "series": [{
            "name": f"{lisn.name} EUT-side |Z|",
            "color": "var(--s-2)",
            "points": [[float(f), z] for f, z in zip(freqs, eut)],
        }],
        "y_label": "ohm",
        "note": (
            f"{_eng(lisn.inductance_h,'H')} network: |Z| is {eut[0]:.2f} Ω at "
            f"9 kHz, not 50 Ω. A filter sized against a flat 50 Ω source "
            f"overestimates its own attenuation at the low end."
        ),
    }


def _filter_labels(design, stages: int, n_lines: int) -> dict:
    """ref -> value label, matching filterComponents() in web/src/ciasFilter.js.

    The schematic component owns the reference designators; this only has to
    agree with them, so the naming below mirrors that file exactly rather than
    inventing a parallel scheme.
    """
    cmc = _eng(design.l_cm_selected_h, "H")
    c_x = _eng(design.c_x_selected_f, "F")
    c_y = _eng(design.c_y_per_line_f, "F")
    labels: dict[str, str] = {}
    for stage in range(1, stages + 1):
        labels[f"CMC{stage}"] = cmc
        if n_lines == 2:
            labels[f"C_X{stage}"] = c_x
            labels[f"C_YL{stage}"] = c_y
            labels[f"C_YN{stage}"] = c_y
        elif n_lines == 3:
            for pair in ("12", "23", "31"):
                labels[f"C_X{stage}_{pair}"] = c_x
            for i in range(1, 4):
                labels[f"C_Y{i}_{stage}"] = c_y
        else:
            for leg in ("1n", "2n", "3n"):
                labels[f"C_X{stage}_{leg}"] = c_x
            for i in range(1, 5):
                labels[f"C_Y{i}_{stage}"] = c_y
    return labels


def _topology(n_lines: int, dc: bool) -> str:
    if n_lines == 2:
        return "dc" if dc else "mains"
    if n_lines == 3:
        return "3ph"
    if n_lines == 4:
        return "3phn"
    raise ValueError(f"n_lines must be 2, 3 or 4 -- got {n_lines}")


def _lisn(model: str):
    if model == "cispr16":
        return cispr16_lisn()
    if model == "cispr25":
        return cispr25_lisn()
    raise ValueError(
        f"unknown LISN {model!r} -- 'cispr16' (50 uH/50 ohm mains) or "
        f"'cispr25' (5 uH automotive)"
    )


def _limit_line(standard: str, emission_class: str, detector: str):
    """The single limit line that applies to a trace measured with `detector`.

    Deliberately returns ONE line, not "all of them": a quasi-peak limit judges
    a quasi-peak measurement. Judging one unknown trace against both the QP and
    AVG lines would manufacture a verdict the standard does not make.
    """
    if standard == "cispr32_mains":
        table = {
            ("A", "quasi_peak"): CISPR32_CLASS_A_MAINS_QP,
            ("A", "average"): CISPR32_CLASS_A_MAINS_AVG,
            ("B", "quasi_peak"): CISPR32_CLASS_B_MAINS_QP,
            ("B", "average"): CISPR32_CLASS_B_MAINS_AVG,
        }
        key = (emission_class.upper(), detector)
        if key not in table:
            raise ValueError(
                f"CISPR 32 mains defines no {detector!r} limit for class "
                f"{emission_class!r}. Classes are A/B; limits are "
                f"'quasi_peak' or 'average'. CISPR 32 publishes no peak limit "
                f"-- to screen a peak scan, judge it against the QP limit and "
                f"pass trace_detector='peak'."
            )
        return table[key]

    if standard == "cispr25_conducted":
        # The argument is a string because CISPR 32 classes are "A"/"B";
        # CISPR 25 numbers its classes, so convert explicitly rather than
        # letting "3" fall through as a non-matching value.
        try:
            numbered = int(str(emission_class).strip())
        except ValueError:
            raise ValueError(
                f"CISPR 25 emission class must be 1..5, got {emission_class!r} "
                f"(A/B are CISPR 32 classes -- wrong standard?)"
            ) from None
        return cispr25_conducted_voltage(numbered, detector)

    raise ValueError(
        f"unknown standard {standard!r} -- use 'cispr32_mains' or 'cispr25_conducted'"
    )


def _decimate(freqs: np.ndarray, levels: np.ndarray):
    """Peak-preserving decimation onto <= MAX_TRACE_POINTS log-frequency bins.

    Returns the ACTUAL (f, level) of the loudest sample in each bin, so a
    clicked point is a real measurement and not an interpolation artifact.
    """
    if freqs.size <= MAX_TRACE_POINTS:
        return freqs, levels

    edges = np.logspace(
        np.log10(freqs[0]), np.log10(freqs[-1]), MAX_TRACE_POINTS + 1
    )
    idx = np.clip(np.searchsorted(edges, freqs, side="right") - 1, 0, MAX_TRACE_POINTS - 1)
    keep = []
    for b in range(MAX_TRACE_POINTS):
        members = np.flatnonzero(idx == b)
        if members.size:
            keep.append(members[np.argmax(levels[members])])
    keep = np.array(keep, dtype=int)
    return freqs[keep], levels[keep]


def _limit_polylines(line):
    """One polyline per segment -- never a line drawn across a coverage gap.

    CISPR 25 defines limits only inside protected bands; joining the segments
    would draw a limit through frequencies where none exists.
    """
    return [
        {
            "f_start_hz": s.f_start_hz,
            "f_stop_hz": s.f_stop_hz,
            "level_start_dbuv": s.level_start_dbuv,
            "level_stop_dbuv": s.level_stop_dbuv,
        }
        for s in line.segments
    ]


# --- tools ------------------------------------------------------------------

@mcp.tool(
    title="Check a spectrum against a limit",
    description=(
        "Judge a conducted-emissions scan against a CISPR limit line and render "
        "an interactive spectrum chart. Accepts the raw text of a spectrum-"
        "analyzer CSV export. Returns the pass/fail verdict, the worst "
        "offenders, and the detected switching frequency. "
        "Pre-compliance estimate only -- not a compliance statement."
    ),
    meta=UI_META,
)
def check_spectrum(
    csv_text: str,
    standard: str = "cispr32_mains",
    emission_class: str = "B",
    detector: str = "quasi_peak",
    level_column: int | None = None,
    trace_detector: str | None = None,
    freq_unit: str | None = None,
    level_unit: str | None = None,
) -> CallToolResult:
    """Check a conducted-emissions scan against a CISPR limit line.

    Args:
        csv_text: Raw contents of the spectrum-analyzer CSV export.
        standard: 'cispr32_mains' (IT/multimedia mains port) or
            'cispr25_conducted' (automotive conducted voltage).
        emission_class: 'A' or 'B' for CISPR 32; '1'..'5' for CISPR 25.
        detector: which LIMIT line to judge against -- 'quasi_peak',
            'average', or 'peak' (CISPR 25 only).
        level_column: 1-based numeric column holding the level. REQUIRED when
            the export carries more than two numeric columns (multi-trace).
        trace_detector: the detector the SCAN was measured with, when it differs
            from the limit's. 'peak' against a QP or AVG limit is the standard
            pre-scan screen: peak >= QP >= AVG always, so passing is definitive
            and exceeding is inconclusive until re-measured. Say so rather than
            reporting a plain FAIL.
        freq_unit: 'Hz'|'kHz'|'MHz'|'GHz' -- only when the header omits it.
        level_unit: 'dBuV'|'dBm' -- only when the header omits it.
    """
    line = _limit_line(standard, emission_class, detector)

    freqs, levels, unit = _with_csv(csv_text, lambda p: read_spectrum_csv(
        p, freq_unit=freq_unit, level_unit=level_unit,
        level_column=level_column, return_unit=True,
    ))

    if unit != "dbuv":
        raise ValueError(
            f"trace is in {unit!r}; {line.name} is a dBuV voltage limit. A "
            f"current-probe (dBuA) scan needs a radiated/current limit, not "
            f"this one."
        )

    covered, limit_levels = line.levels_where_covered(freqs)
    if not covered.any():
        raise ValueError(
            f"no sample in the scan ({freqs[0]/1e6:.4g}-{freqs[-1]/1e6:.4g} MHz) "
            f"falls inside {line.name} coverage -- wrong standard for this scan?"
        )

    margins = np.full(freqs.shape, np.nan)
    margins[covered] = limit_levels[covered] - levels[covered]

    worst_i = int(np.nanargmin(margins))
    worst_margin = float(margins[worst_i])

    # Detector ordering is physics: peak >= quasi_peak >= average for any
    # signal. So a peak trace under a QP limit is a DEFINITIVE pass, while a
    # peak trace over it is INCONCLUSIVE -- the QP re-measure may well pass.
    # Reporting that as a bare "FAIL" would be a false negative dressed up as
    # a result.
    _STRICTNESS = {"peak": 2, "quasi_peak": 1, "average": 0}
    screening = (
        trace_detector is not None
        and trace_detector != line.detector
        and _STRICTNESS.get(trace_detector, 0) > _STRICTNESS.get(line.detector, 0)
    )
    if worst_margin >= 0.0:
        verdict = "PASS"
    else:
        verdict = "INCONCLUSIVE" if screening else "FAIL"

    # Worst offenders: the most-exceeding points, thinned so a single broad
    # hump does not fill the table with its own neighbours.
    order = np.argsort(margins[covered])
    covered_idx = np.flatnonzero(covered)
    offenders, taken = [], []
    for k in order:
        i = int(covered_idx[k])
        if margins[i] > 0.0 and len(offenders) >= 3:
            break
        if any(abs(np.log10(freqs[i]) - np.log10(freqs[j])) < 0.02 for j in taken):
            continue
        taken.append(i)
        offenders.append({
            "f_hz": float(freqs[i]),
            "level_dbuv": float(levels[i]),
            "limit_dbuv": float(limit_levels[i]),
            "margin_db": float(margins[i]),
        })
        if len(offenders) >= 8:
            break

    comb = detect_comb(freqs, levels)
    holes = [
        {"f_lo_hz": float(a), "f_hi_hz": float(b)}
        for a, b in unswept_regions_sampled(line, freqs)
    ]

    d_f, d_l = _decimate(freqs, levels)

    # --- the two channels ---------------------------------------------------
    # `content` is what the MODEL reads: a digest, never the arrays.
    # `structuredContent` is what the WIDGET renders.
    head = (
        f"{verdict} vs {line.name}. Worst margin {worst_margin:+.1f} dB at "
        f"{freqs[worst_i] / 1e6:.4g} MHz "
        f"({levels[worst_i]:.1f} dBuV vs {limit_levels[worst_i]:.1f} limit)."
    )
    lines = [head, f"{freqs.size} points, {freqs[0]/1e6:.4g}-{freqs[-1]/1e6:.4g} MHz."]
    if screening:
        lines.append(
            f"Screening only: a {trace_detector} trace judged against a "
            f"{line.detector} limit. {trace_detector} >= {line.detector} always, "
            f"so points under the limit definitively pass; points over it are "
            f"INCONCLUSIVE until re-measured with a {line.detector} detector."
        )
    if comb.found:
        lines.append(
            f"Harmonic comb detected: f_sw ~ {comb.f_sw_hz / 1e3:.4g} kHz "
            f"(confidence {comb.confidence:.2f}, {len(comb.harmonics)} harmonics)."
        )
    if holes:
        lines.append(
            f"WARNING: {len(holes)} unswept region(s) inside the limit's "
            f"coverage -- the scan does not measure the whole regulated range."
        )
    if offenders and offenders[0]["margin_db"] < 0:
        over = [o for o in offenders if o["margin_db"] < 0][:4]
        lines.append("Offenders: " + "; ".join(
            f"{o['f_hz']/1e6:.4g} MHz {o['margin_db']:+.1f} dB" for o in over))
    lines.append("Pre-compliance estimate only -- not a compliance statement.")

    # Returning a CallToolResult keeps the two channels genuinely separate:
    # FastMCP passes it through verbatim instead of JSON-dumping the whole
    # payload into `content`. Without this the 900-point trace lands in the
    # model's context on every call.
    return CallToolResult(
        content=[TextContent(type="text", text="\n".join(lines))],
        structuredContent={
        "verdict": verdict,
        "limit_name": line.name,
        "detector": line.detector,
        "trace_detector": trace_detector,
        "screening": screening,
        "worst": {
            "f_hz": float(freqs[worst_i]),
            "level_dbuv": float(levels[worst_i]),
            "limit_dbuv": float(limit_levels[worst_i]),
            "margin_db": worst_margin,
        },
        "trace": {
            "f_hz": [float(x) for x in d_f],
            "level_dbuv": [float(x) for x in d_l],
            "decimated_from": int(freqs.size),
        },
        "limit_segments": _limit_polylines(line),
        "offenders": offenders,
        "f_sw_hz": float(comb.f_sw_hz) if comb.found else None,
        "unswept": holes,
        },
    )


@mcp.tool(
    title="Inspect a spectrum CSV",
    description=(
        "Report the numeric columns in a spectrum-analyzer export and which one "
        "holds the frequency. Call this FIRST on any multi-trace file so you can "
        "pass the right level_column, instead of guessing."
    ),
)
def inspect_spectrum_csv(csv_text: str) -> dict:
    """List the numeric columns of a spectrum CSV and their header names."""
    count, freq_col, names = _with_csv(csv_text, spectrum_csv_columns)
    return {
        "numeric_columns": count,
        "frequency_column": freq_col,
        "column_names": [n or "" for n in names],
        "level_column_required": count > 2,
        "hint": (
            "Pass level_column (1-based) to check_spectrum."
            if count > 2 else
            "Two columns only -- level_column is inferred."
        ),
    }


@mcp.tool(
    title="Detect switching frequency",
    description=(
        "Find the harmonic comb in a measured spectrum and estimate the "
        "converter's switching frequency, with the matched harmonics and any "
        "peaks that do NOT belong to the comb."
    ),
    meta=UI_CURVES_META,
)
def detect_switching_frequency(
    csv_text: str,
    level_column: int | None = None,
    f_sw_min_khz: float = 20.0,
    f_sw_max_khz: float = 5000.0,
) -> CallToolResult:
    """Estimate f_sw from the harmonic comb in a spectrum."""
    freqs, levels = _with_csv(csv_text, lambda p: read_spectrum_csv(
        p, level_column=level_column))
    comb = detect_comb(freqs, levels,
                       f_sw_min_hz=f_sw_min_khz * 1e3,
                       f_sw_max_hz=f_sw_max_khz * 1e3)

    d_f, d_l = _decimate(freqs, levels)
    series = [{"name": "Measured", "points": [[float(a), float(b)]
                                              for a, b in zip(d_f, d_l)],
               "color": "var(--s-2)"}]
    markers = [{"x": float(h.frequency_hz), "y": float(h.level_dbuv)}
               for h in comb.harmonics]

    if not comb.found:
        summary = (
            "No harmonic comb found between "
            f"{f_sw_min_khz:g} and {f_sw_max_khz:g} kHz. That is a real result, "
            "not a failure: spread-spectrum, quasi-resonant and jittered "
            "converters deliberately have no clean comb."
        )
        subtitle = "no comb detected"
    else:
        summary = (
            f"f_sw ~ {comb.f_sw_hz / 1e3:.4g} kHz "
            f"(confidence {comb.confidence:.2f}, coverage {comb.coverage:.2f}, "
            f"{len(comb.harmonics)} harmonics matched). "
            f"{len(comb.residual_peaks)} peak(s) do not fit the comb."
        )
        subtitle = (f"f_sw ≈ {comb.f_sw_hz / 1e3:.4g} kHz · "
                    f"{len(comb.harmonics)} harmonics")

    return _curves_result(
        title="Harmonic comb", subtitle=subtitle,
        y_label="dBµV", y_unit="dBµV", series=series, markers=markers,
        summary=summary,
    )


@mcp.tool(
    title="Get a limit line",
    description=(
        "The limit line itself, as frequency/level segments — no measurement "
        "needed. Covers CISPR 32 mains conducted, CISPR 25 conducted, and "
        "CISPR 32 radiated. Use it to answer 'what IS the limit at X MHz?'."
    ),
    meta=UI_CURVES_META,
)
def get_limit_line(
    standard: str = "cispr32_mains",
    emission_class: str = "B",
    detector: str = "quasi_peak",
    distance_m: float = 10.0,
) -> CallToolResult:
    """Return a limit line's segments.

    Args:
        standard: 'cispr32_mains', 'cispr25_conducted', or 'cispr32_radiated'.
        emission_class: 'A'/'B' (CISPR 32) or '1'..'5' (CISPR 25).
        detector: 'quasi_peak', 'average', or 'peak' (CISPR 25 only).
        distance_m: measurement distance, radiated only (3 or 10 m).
    """
    if standard == "cispr32_radiated":
        line = cispr32_radiated(emission_class.upper(), distance_m)
        unit, y_label = "dBµV/m", "dBµV/m"
    else:
        line = _limit_line(standard, emission_class, detector)
        unit, y_label = "dBµV", "dBµV"

    rows = [
        f"{s.f_start_hz/1e6:.4g}-{s.f_stop_hz/1e6:.4g} MHz: "
        f"{s.level_start_dbuv:.1f}"
        + (f"->{s.level_stop_dbuv:.1f}" if s.level_stop_dbuv != s.level_start_dbuv else "")
        + f" {unit}"
        for s in line.segments
    ]
    summary = f"{line.name} ({line.detector}), {len(line.segments)} segment(s):\n" + \
              "\n".join(rows)

    return _curves_result(
        title=line.name, subtitle=f"{len(line.segments)} segment(s) · {line.detector}",
        y_label=y_label, y_unit=unit, series=[_limit_series(line, "Limit")],
        summary=summary,
        note=("Segments are drawn separately: between them the standard "
              "defines no limit." if len(line.segments) > 1 else None),
    )


@mcp.tool(
    title="LISN impedance",
    description=(
        "EUT-side impedance of a LISN versus frequency — the source impedance "
        "your filter actually works into, which is NOT 50 ohm at low frequency."
    ),
    meta=UI_CURVES_META,
)
def lisn_impedance(
    model: str = "cispr16",
    f_min_hz: float = 9e3,
    f_max_hz: float = 30e6,
) -> CallToolResult:
    """Impedance of a LISN seen by the equipment under test.

    Args:
        model: 'cispr16' (50 uH/50 ohm, mains) or 'cispr25' (5 uH, automotive).
    """
    lisn = _lisn(model)
    freqs = _sample_log(f_min_hz, f_max_hz)
    eut = np.abs([lisn.eut_impedance(f) for f in freqs])
    branch = np.abs([lisn.measuring_branch_impedance(f) for f in freqs])

    series = [
        {"name": "EUT-side |Z|", "points": [[float(f), float(z)]
                                            for f, z in zip(freqs, eut)],
         "color": "var(--s-2)"},
        {"name": "Measuring branch |Z|", "points": [[float(f), float(z)]
                                                    for f, z in zip(freqs, branch)],
         "color": "var(--s-1)", "style": "dashed"},
    ]
    lo, hi = float(eut[0]), float(eut[-1])
    summary = (
        f"{lisn.name}: EUT-side |Z| rises from {lo:.2f} ohm at "
        f"{freqs[0]/1e3:.4g} kHz to {hi:.1f} ohm at {freqs[-1]/1e6:.4g} MHz "
        f"(L = {lisn.inductance_h*1e6:.3g} uH, C = "
        f"{lisn.coupling_capacitance_f*1e6:.3g} uF, "
        f"{lisn.measuring_impedance_ohm:.0f} ohm receiver). "
        f"Treating this as a flat 50 ohm overestimates attenuation at the low end."
    )
    return _curves_result(
        title=lisn.name, subtitle="impedance vs frequency",
        y_label="ohm", y_unit="ohm", series=series, summary=summary,
    )


@mcp.tool(
    title="LISN SPICE model",
    description=(
        "SPICE subcircuit for a LISN, to drop into a simulation so the filter "
        "is evaluated against the real network instead of a 50 ohm resistor."
    ),
)
def lisn_spice_model(model: str = "cispr16", name: str | None = None,
                     terminated: bool = True) -> dict:
    """Export a LISN as a SPICE subcircuit.

    Args:
        model: 'cispr16' or 'cispr25'.
        name: subcircuit name; defaults to the LISN's own.
        terminated: include the 50 ohm receiver termination.
    """
    lisn = _lisn(model)
    return {
        "lisn": lisn.name,
        "inductance_uh": lisn.inductance_h * 1e6,
        "coupling_capacitance_uf": lisn.coupling_capacitance_f * 1e6,
        "measuring_impedance_ohm": lisn.measuring_impedance_ohm,
        "terminated": terminated,
        "spice": lisn.to_spice_subckt(name=name, terminated=terminated),
    }


@mcp.tool(
    title="List available limit standards",
    description=(
        "The limit lines Hertz can judge against, with the emission classes and "
        "detectors each one defines. Call this when unsure which arguments "
        "check_spectrum will accept."
    ),
)
def list_standards() -> dict:
    """Enumerate the supported standards, classes, and detectors."""
    return {
        "standards": [
            {
                "id": "cispr32_mains",
                "title": "CISPR 32 mains port, conducted voltage",
                "emission_classes": ["A", "B"],
                "detectors": ["quasi_peak", "average"],
                "range_mhz": [0.15, 30.0],
                "note": "No peak limit line exists; a peak scan is a pre-scan proxy.",
            },
            {
                "id": "cispr25_conducted",
                "title": "CISPR 25 conducted voltage (automotive)",
                "emission_classes": ["1", "2", "3", "4", "5"],
                "detectors": ["peak", "quasi_peak", "average"],
                "note": (
                    "Limits exist only inside protected bands -- frequencies "
                    "between bands are genuinely unregulated, not missing."
                ),
            },
        ]
    }


@mcp.tool(
    title="Filter requirements from a measurement",
    description=(
        "Turn a measured level and its limit into the attenuation the filter "
        "must provide, and the frequency to design at (ANP015 step 1). Run this "
        "before design_filter."
    ),
)
def filter_requirements(
    f_sw_hz: float,
    measured_dbuv: float,
    limit_dbuv: float,
    margin_db: float = 10.0,
    stages: int = 1,
) -> dict:
    """Required attenuation, design frequency and resulting cutoff.

    Args:
        f_sw_hz: converter switching frequency.
        measured_dbuv: the offending measured level.
        limit_dbuv: the limit at that frequency.
        margin_db: design buffer above the limit (ANP015 uses 10 dB).
        stages: 1 or 2 filter stages.
    """
    a_req = required_attenuation_db(measured_dbuv, limit_dbuv, margin_db)
    f_design = design_frequency(f_sw_hz)
    f_co = cutoff_frequency(f_design, a_req, stages)
    return {
        "required_attenuation_db": a_req,
        "design_frequency_hz": f_design,
        "cutoff_frequency_hz": f_co,
        "stages": stages,
        "note": (
            f"Design frequency is f_sw ({f_sw_hz/1e3:.4g} kHz) or its first "
            f"harmonic at/above 150 kHz -- here {f_design/1e3:.4g} kHz. "
            f"A_req = measured - limit + margin."
        ),
    }


@mcp.tool(
    title="Design a line filter (ANP015)",
    description=(
        "Size a CM+DM line filter to a required attenuation, per Wurth "
        "Elektronik application note ANP015. Rounds onto the component values "
        "you actually have, and returns the insertion-loss curves. Supports "
        "single-phase, DC, 3-phase and 3-phase+neutral."
    ),
    meta=UI_FILTER_META,
)
def design_filter(
    f_sw_hz: float,
    a_req_cm_db: float,
    a_req_dm_db: float,
    c_y_per_line_nf: float = 2.2,
    l_dm_uh: float = 10.0,
    stages: int = 1,
    n_lines: int = 2,
    dc_supply: bool = False,
    bindings: dict | None = None,
    l_cm_candidates_mh: list[float] | None = None,
    c_x_candidates_uf: list[float] | None = None,
) -> CallToolResult:
    """Full ANP015 sizing pass with component rounding.

    Args:
        f_sw_hz: switching frequency.
        a_req_cm_db: required common-mode attenuation.
        a_req_dm_db: required differential-mode attenuation.
        c_y_per_line_nf: Y capacitance per line, nF (safety-limited).
        l_dm_uh: DM (leakage) inductance of the CM choke, uH.
        stages: 1 or 2.
        n_lines: 2 = single-phase L/N or DC pair, 3 = 3-phase delta,
            4 = 3-phase + neutral (star).
        l_cm_candidates_mh: available CM choke values in mH; defaults to E6.
        c_x_candidates_uf: available X-cap values in uF; defaults to E6.
    """
    l_cm_candidates = (
        [v * 1e-3 for v in l_cm_candidates_mh] if l_cm_candidates_mh
        else utils.series_candidates((1.0, 1.5, 2.2, 3.3, 4.7, 6.8),
                                     decade_min=-6, decade_max=-1)
    )
    c_x_candidates = (
        [v * 1e-6 for v in c_x_candidates_uf] if c_x_candidates_uf
        else utils.series_candidates((1.0, 1.5, 2.2, 3.3, 4.7, 6.8),
                                     decade_min=-9, decade_max=-4)
    )

    d = design_line_filter(
        f_sw_hz=f_sw_hz,
        a_req_cm_db=a_req_cm_db,
        a_req_dm_db=a_req_dm_db,
        c_y_per_line_f=c_y_per_line_nf * 1e-9,
        l_dm_h=l_dm_uh * 1e-6,
        stages=stages,
        l_cm_candidates=l_cm_candidates,
        c_x_candidates=c_x_candidates,
        n_lines=n_lines,
    )

    # Insertion loss of what was actually SELECTED, in both modes, against the
    # reference impedances the app note uses (25 ohm CM, 100 ohm DM).
    f_lo, f_hi = 150e3, 30e6
    fc, cm_std, cm_wc = network.insertion_loss_curves(
        d.l_cm_selected_h, d.c_yg_f, stages, 25.0, f_lo, f_hi)
    fd, dm_std, dm_wc = network.insertion_loss_curves(
        d.l_dm_h, d.c_x_selected_f * d.c_x_dm_factor, stages, 100.0, f_lo, f_hi)

    series = [
        {"name": "CM IL (25 Ω)", "points": [[float(a), float(b)] for a, b in zip(fc, cm_std)],
         "color": "var(--s-2)"},
        {"name": "CM worst case", "points": [[float(a), float(b)] for a, b in zip(fc, cm_wc)],
         "color": "var(--s-2)", "style": "dashed"},
        {"name": "DM IL (100 Ω)", "points": [[float(a), float(b)] for a, b in zip(fd, dm_std)],
         "color": "var(--s-1)"},
        {"name": "DM worst case", "points": [[float(a), float(b)] for a, b in zip(fd, dm_wc)],
         "color": "var(--s-1)", "style": "dashed"},
    ]

    lines = [
        f"{stages}-stage, {n_lines}-line filter designed at "
        f"CM {d.f_design_cm_hz/1e3:.4g} kHz / DM {d.f_design_dm_hz/1e3:.4g} kHz.",
        f"CM: L required {d.l_cm_required_h*1e3:.3g} mH -> selected "
        f"{d.l_cm_selected_h*1e3:.3g} mH; C_YG {d.c_yg_f*1e9:.3g} nF; "
        f"f_co {d.f_cutoff_cm_hz/1e3:.4g} kHz; achieved "
        f"{d.attenuation_cm_db:.1f} dB (needed {a_req_cm_db:.1f}).",
        f"DM: L_DM {d.l_dm_h*1e6:.3g} uH; C_X required {d.c_x_required_f*1e6:.3g} uF "
        f"-> selected {d.c_x_selected_f*1e6:.3g} uF (x{d.c_x_dm_factor:g} DM factor); "
        f"f_co {d.f_cutoff_dm_hz/1e3:.4g} kHz; achieved "
        f"{d.attenuation_dm_db:.1f} dB (needed {a_req_dm_db:.1f}).",
    ]
    if d.l_cm_floor_from_leakage:
        lines.append(
            "CM inductance was raised to keep the choke's leakage inductance "
            "consistent with the DM design -- the two are not independent."
        )
    shortfall = [m for m, got, need in
                 (("CM", d.attenuation_cm_db, a_req_cm_db),
                  ("DM", d.attenuation_dm_db, a_req_dm_db)) if got < need]
    if shortfall:
        lines.append(
            f"WARNING: {'/'.join(shortfall)} attenuation falls short after "
            f"rounding onto the available values -- widen the candidate list "
            f"or add a stage."
        )
    lines.append("Pre-compliance estimate only -- not a compliance statement.")

    topology = _topology(n_lines, dc_supply)
    labels = _filter_labels(d, stages, n_lines)
    bound = {k: v for k, v in (bindings or {}).items() if v and v.get("mpn")}

    # BOM pane: one row per position, quantity-collapsed by (ref-family, value).
    bom = []
    for ref, label in labels.items():
        kind = "cmc" if ref.startswith("CMC") else ("cx" if ref.startswith("C_X") else "cy")
        b = bound.get(ref) or {}
        bom.append({
            "ref": ref, "kind": kind, "value": label,
            "mpn": b.get("mpn"), "manufacturer": b.get("manufacturer"),
            "status": "bound" if b.get("mpn") else "unbound",
        })
    n_bound = sum(1 for r in bom if r["status"] == "bound")

    lines.append("Components: " + ", ".join(f"{r}={v}" for r, v in labels.items()))
    if bound:
        lines.append(
            f"{n_bound}/{len(bom)} positions bound to catalog parts: "
            + ", ".join(f"{r}={b['mpn']}" for r, b in bound.items()) +
            ". Values above are the DESIGN targets -- a bound part's actual "
            "value and measured impedance may differ, so re-read the margins."
        )
    else:
        lines.append(
            f"No parts bound yet -- {len(bom)} positions are design values only. "
            "Use list_candidates, or click a component in the schematic."
        )

    return CallToolResult(
        content=[TextContent(type="text", text="\n".join(lines))],
        structuredContent={
            "title": f"{stages}-stage line filter",
            "subtitle": (f"CM {_eng(d.l_cm_selected_h,'H')} / {_eng(d.c_yg_f,'F')} · "
                         f"DM {_eng(d.l_dm_h,'H')} / {_eng(d.c_x_selected_f,'F')}"),
            "y_label": "Insertion loss (dB)",
            "y_unit": "dB",
            "series": series,
            "note": ("Dashed = CISPR 17 worst case (0.1 Ω/100 Ω), the "
                     "pessimistic bound; solid = the reference impedance."),
            # Schematic pane — reference designators match filterComponents()
            # in web/src/ciasFilter.js, which the widget's FilterSchematic owns.
            "stages": stages,
            "topology": topology,
            "labels": labels,
            "bindings": bound,
            "bom": bom,
            # Targets the widget passes straight to list_candidates when a
            # component is clicked, so the picker needs no extra round trip.
            "targets": {
                "cmc": d.l_cm_selected_h,
                "cx": d.c_x_selected_f,
                "cy": d.c_y_per_line_f,
            },
            # `values` pane: the ANP015 sizing trail, so the numbers behind the
            # component values are inspectable rather than taken on trust.
            "values": [
                {"k": "Stages", "v": str(stages)},
                {"k": "Lines", "v": f"{n_lines} ({topology})"},
                {"k": "Design frequency CM", "v": f"{d.f_design_cm_hz/1e3:.4g} kHz"},
                {"k": "Design frequency DM", "v": f"{d.f_design_dm_hz/1e3:.4g} kHz"},
                {"k": "Cutoff CM", "v": f"{d.f_cutoff_cm_hz/1e3:.4g} kHz"},
                {"k": "Cutoff DM", "v": f"{d.f_cutoff_dm_hz/1e3:.4g} kHz"},
                {"k": "L_CM required", "v": _eng(d.l_cm_required_h, "H")},
                {"k": "L_CM selected", "v": _eng(d.l_cm_selected_h, "H")},
                {"k": "C_YG (per pair)", "v": _eng(d.c_yg_f, "F")},
                {"k": "C_Y per line", "v": _eng(d.c_y_per_line_f, "F")},
                {"k": "L_DM (leakage)", "v": _eng(d.l_dm_h, "H")},
                {"k": "C_X required", "v": _eng(d.c_x_required_f, "F")},
                {"k": "C_X selected", "v": _eng(d.c_x_selected_f, "F")},
                {"k": "X DM factor", "v": f"×{d.c_x_dm_factor:g}"},
                {"k": "Attenuation CM", "v": f"{d.attenuation_cm_db:.1f} dB "
                                             f"(needed {a_req_cm_db:.1f})"},
                {"k": "Attenuation DM", "v": f"{d.attenuation_dm_db:.1f} dB "
                                             f"(needed {a_req_dm_db:.1f})"},
                {"k": "L_CM floored by leakage",
                 "v": "yes" if d.l_cm_floor_from_leakage else "no"},
            ],
            # `lisn` pane: the source impedance this filter actually works
            # into. Automotive designs are benched on the 5 µH network, mains
            # on the 50 µH one.
            "lisn": _lisn_series("cispr25" if dc_supply else "cispr16"),
        },
    )


@mcp.tool(
    title="Filter insertion loss",
    description=(
        "Insertion loss of an explicit LC filter versus frequency, including "
        "capacitor ESL/ESR and the CISPR 17 worst-case terminations. Use to "
        "check a filter you already have rather than designing a new one."
    ),
    meta=UI_CURVES_META,
)
def filter_insertion_loss(
    inductance_uh: float,
    capacitance_uf: float,
    stages: int = 1,
    reference_impedance_ohm: float = 50.0,
    f_min_hz: float = 150e3,
    f_max_hz: float = 30e6,
    cap_esl_nh: float = 0.0,
    cap_esr_mohm: float = 0.0,
) -> CallToolResult:
    """Insertion-loss curves for a given L/C filter.

    Args:
        reference_impedance_ohm: 25 for CM, 100 for DM, 50 for data-sheet.
        cap_esl_nh: capacitor equivalent series inductance, nH — this is what
            makes real insertion loss roll back over at high frequency.
        cap_esr_mohm: capacitor equivalent series resistance, milliohm.
    """
    freqs, std, wc = network.insertion_loss_curves(
        inductance_uh * 1e-6, capacitance_uf * 1e-6, stages,
        reference_impedance_ohm, f_min_hz, f_max_hz,
    )
    series = [
        {"name": f"IL ({reference_impedance_ohm:g} Ω)",
         "points": [[float(a), float(b)] for a, b in zip(freqs, std)],
         "color": "var(--s-2)"},
        {"name": "CISPR 17 worst case",
         "points": [[float(a), float(b)] for a, b in zip(freqs, wc)],
         "color": "var(--s-limit)", "style": "dashed"},
    ]

    note = None
    if cap_esl_nh > 0 or cap_esr_mohm > 0:
        esl_curve = []
        for f in freqs:
            abcd = network.lc_filter_abcd(
                float(f), inductance_uh * 1e-6, capacitance_uf * 1e-6, stages,
                cap_esl_h=cap_esl_nh * 1e-9, cap_esr_ohm=cap_esr_mohm * 1e-3,
            )
            esl_curve.append(float(network.insertion_loss_db(
                abcd, reference_impedance_ohm, reference_impedance_ohm)))
        series.append({
            "name": f"with ESL {cap_esl_nh:g} nH",
            "points": [[float(a), b] for a, b in zip(freqs, esl_curve)],
            "color": "var(--s-3)",
        })
        note = ("ESL turns the capacitor inductive above its self-resonance, "
                "so the real curve stops following the ideal -20n dB/decade.")

    f_res = 1.0 / (2 * np.pi * np.sqrt(inductance_uh * 1e-6 * capacitance_uf * 1e-6))
    summary = (
        f"{stages}-stage {inductance_uh:g} µH / {capacitance_uf:g} µF, "
        f"LC resonance {f_res/1e3:.4g} kHz. "
        f"At {f_max_hz/1e6:.4g} MHz: {std[-1]:.1f} dB nominal, "
        f"{wc[-1]:.1f} dB CISPR 17 worst case. "
        f"Worst case is the number to design to -- it is up to "
        f"{max(std - wc):.1f} dB below nominal here."
    )
    return _curves_result(
        title="Insertion loss",
        subtitle=f"{stages}-stage · {inductance_uh:g} µH / {capacitance_uf:g} µF",
        y_label="Insertion loss (dB)", y_unit="dB", series=series,
        summary=summary, note=note,
    )


@mcp.tool(
    title="List catalog parts for a filter component",
    description=(
        "Real orderable parts near a target value, for one position in the "
        "filter. Called by the schematic widget when an engineer clicks a "
        "component, and directly when you need to know what exists."
    ),
)
def list_candidates(
    kind: str,
    target_value: float,
    tolerance_pct: float = 60.0,
    manufacturer: str | None = None,
    min_current_a: float = 0.0,
    min_voltage_v: float = 0.0,
    limit: int = 12,
) -> CallToolResult:
    """Catalog parts near a target value.

    Args:
        kind: 'cmc' (common-mode choke), 'cx' (X capacitor, line-to-line) or
            'cy' (Y capacitor, line-to-earth).
        target_value: target in base SI units — henries for 'cmc', farads for
            'cx'/'cy'.
        tolerance_pct: how far either side of target to search.
        min_current_a: reject chokes rated below this (0 keeps unrated parts).
        min_voltage_v: reject capacitors rated below this.
    """
    if kind not in ("cmc", "cx", "cy"):
        raise ValueError(f"kind must be 'cmc', 'cx' or 'cy' -- got {kind!r}")
    rows = _candidate_rows(kind, target_value, tolerance_pct, manufacturer,
                           min_current_a, min_voltage_v, limit)
    unit = "H" if kind == "cmc" else "F"
    if not rows:
        pool = _catalog("cmc" if kind == "cmc" else "cap")
        span = [p.get("inductanceH") if kind == "cmc" else p.get("capacitanceF")
                for p in pool]
        span = [v for v in span if v]
        note = (
            f"No {kind} in the catalog within {tolerance_pct:g}% of "
            f"{_eng(target_value, unit)}"
            + (f" rated >= {min_current_a:g} A" if min_current_a else "")
            + (f" rated >= {min_voltage_v:g} V" if min_voltage_v else "")
            + ". "
            + (f"The catalog holds {len(pool)} {kind} part(s) spanning "
               f"{_eng(min(span), unit)}–{_eng(max(span), unit)}. " if span else "")
            + "Widen the tolerance, relax the rating filter, or accept that "
              "this value must be built from a series/parallel combination."
        )
        payload = {"kind": kind, "target": target_value,
                   "target_label": _eng(target_value, unit),
                   "count": 0, "parts": [], "note": note}
    else:
        best = rows[0]
        note = (f"Closest is {best['mpn']} ({best['manufacturer']}) at "
                f"{best['value']}, {best['deviation_pct']:+.1f}% from target.")
        payload = {"kind": kind, "target": target_value,
                   "target_label": _eng(target_value, unit),
                   "count": len(rows), "parts": rows, "note": note}

    # CallToolResult, not a plain dict: FastMCP only populates
    # structuredContent for a pass-through result, and the widget reads the
    # part list from there.
    return CallToolResult(
        content=[TextContent(type="text", text=note)],
        structuredContent=payload,
    )


@mcp.tool(
    title="Filter safety checks",
    description=(
        "Y-capacitor earth-leakage current and X-capacitor discharge-resistor "
        "sizing for a mains filter (ANP015). These are the checks that decide "
        "whether a filter is legal to ship, not just effective."
    ),
)
def check_filter_safety(
    v_grid_rms: float = 230.0,
    f_grid_hz: float = 50.0,
    c_y_line_nf: float = 2.2,
    c_y_neutral_nf: float = 2.2,
    c_x_total_uf: float = 0.47,
    v_safe: float = 60.0,
    t_max_s: float = 1.0,
) -> dict:
    """Earth-leakage current and X-cap discharge resistor.

    Args:
        v_safe: voltage the X-caps must be below after t_max_s (IEC 60335: 60 V).
        t_max_s: time allowed to discharge after mains disconnection.
    """
    leakage_a = y_capacitor_leakage_current(
        v_grid_rms, f_grid_hz, c_y_line_nf * 1e-9, c_y_neutral_nf * 1e-9,
        c_x_total_uf * 1e-6,
    )
    v_peak = v_grid_rms * np.sqrt(2.0)
    r_max = max_discharge_resistance(c_x_total_uf * 1e-6, v_peak, v_safe, t_max_s)
    p_r = discharge_resistor_power(v_grid_rms, r_max)
    return {
        "y_cap_leakage_current_ma": leakage_a * 1e3,
        "leakage_note": (
            "Compare against your product standard's touch-current limit "
            "(commonly 0.25-3.5 mA depending on class and appliance type). "
            "Hertz does not pick the limit for you."
        ),
        "x_cap_discharge": {
            "max_resistance_kohm": r_max / 1e3,
            "start_voltage_v": float(v_peak),
            "target_voltage_v": v_safe,
            "time_s": t_max_s,
            "continuous_dissipation_mw": p_r * 1e3,
        },
        "note": (
            f"Discharge resistor must be <= {r_max/1e3:.3g} kohm to reach "
            f"{v_safe:g} V within {t_max_s:g} s from the {v_peak:.0f} V peak; "
            f"it then burns {p_r*1e3:.1f} mW continuously."
        ),
    }


@mcp.tool(
    title="Input filter stability (Middlebrook)",
    description=(
        "Middlebrook check: does the filter's output-impedance peak stay clear "
        "of the converter's negative input resistance? A filter that passes EMI "
        "and fails this will oscillate."
    ),
)
def check_input_filter_stability(
    inductance_uh: float,
    capacitance_uf: float,
    v_in_min: float,
    p_in_w: float,
) -> dict:
    """Middlebrook input-filter interaction check and damping guidance."""
    r = input_filter_interaction(
        inductance_uh * 1e-6, capacitance_uf * 1e-6, v_in_min, p_in_w)
    ok = r.margin_db > 0
    return {
        "resonance_hz": r.resonance_hz,
        "filter_characteristic_impedance_ohm": r.characteristic_impedance_ohm,
        "converter_input_impedance_ohm": r.converter_input_impedance_ohm,
        "margin_db": r.margin_db,
        "stable": ok,
        "damping": {
            "resistor_ohm": r.damping_resistor_ohm,
            "capacitor_min_uf": r.damping_capacitor_min_f * 1e6,
            "capacitor_max_uf": r.damping_capacitor_max_f * 1e6,
        },
        "note": (
            f"Filter resonates at {r.resonance_hz/1e3:.4g} kHz with R0 = "
            f"{r.characteristic_impedance_ohm:.2f} ohm against a converter input "
            f"impedance of {r.converter_input_impedance_ohm:.2f} ohm "
            f"(-{abs(r.converter_input_impedance_ohm):.2f} ohm incremental). "
            + ("Margin is positive -- stable as designed."
               if ok else
               "NEGATIVE margin: add the damping branch below, or the loop "
               "will oscillate at the filter resonance.")
        ),
    }


@mcp.tool(
    title="CISPR 16-1-1 receiver emulation",
    description=(
        "Run a sampled time-domain waveform through a compliant measuring "
        "receiver — peak, quasi-peak and average — instead of reading an FFT. "
        "Quasi-peak weighting is what the standard actually judges."
    ),
    meta=UI_CURVES_META,
)
def measure_receiver(
    csv_text: str,
    sample_rate_hz: float,
    band: str | None = None,
    detectors: list[str] | None = None,
) -> CallToolResult:
    """Emulate an EMI receiver on a sampled waveform.

    Args:
        csv_text: CSV of the waveform; one amplitude column in volts, or two
            columns (time, amplitude) — the time column is ignored, the
            sample rate is taken from sample_rate_hz.
        sample_rate_hz: sampling rate of the waveform.
        band: CISPR band 'A'..'D'; defaults to the band containing the middle
            of the analysed range.
        detectors: any of 'peak', 'quasi_peak', 'average'.
    """
    dets = tuple(detectors or ("peak", "quasi_peak", "average"))
    cols = [c.strip() for c in csv_text.splitlines()[0].replace(";", ",").split(",")]
    n_cols = 2 if len(cols) >= 2 else 1
    parsed = _numeric_csv(csv_text, n_cols, "amplitude (V), optionally time first")
    x = parsed[-1]

    chosen = band or "B"
    band_obj = band_for_frequency({"A": 100e3, "B": 1e6, "C": 60e6, "D": 500e6}
                                  .get(chosen.upper(), 1e6))
    result = measure(x, sample_rate_hz, band_obj, detectors=dets)

    freqs = np.asarray(result["frequencies_hz"], dtype=float)
    keep = (freqs >= band_obj.f_min_hz) & (freqs <= band_obj.f_max_hz)
    if not keep.any():
        raise ValueError(
            f"no FFT bin of this waveform falls inside CISPR band "
            f"{band_obj.name} ({band_obj.f_min_hz/1e3:.4g}-"
            f"{band_obj.f_max_hz/1e6:.4g} MHz) at fs = "
            f"{sample_rate_hz/1e6:.4g} MS/s -- wrong band, or sample rate too low."
        )

    series, peaks = [], []
    colors = {"peak": "var(--s-limit)", "quasi_peak": "var(--s-2)", "average": "var(--s-1)"}
    for det in dets:
        key = f"{det}_dbuv"
        if key not in result:
            continue
        lv = np.asarray(result[key], dtype=float)[keep]
        fk = freqs[keep]
        d_f, d_l = _decimate(fk, lv)
        series.append({"name": det, "color": colors.get(det, "var(--s-3)"),
                       "points": [[float(a), float(b)] for a, b in zip(d_f, d_l)]})
        peaks.append(f"{det} max {lv.max():.1f} dBµV at {fk[int(np.argmax(lv))]/1e6:.4g} MHz")

    summary = (
        f"CISPR band {band_obj.name} ({band_obj.f_min_hz/1e3:.4g} kHz-"
        f"{band_obj.f_max_hz/1e6:.4g} MHz, {band_obj.rbw_6db_hz/1e3:g} kHz RBW), "
        f"{x.size} samples at {sample_rate_hz/1e6:.4g} MS/s.\n" + "\n".join(peaks) +
        "\nQuasi-peak sits between peak and average by construction; the gap is "
        "the pulse repetition rate telling you what kind of noise this is."
    )
    return _curves_result(
        title=f"Receiver readings — band {band_obj.name}",
        subtitle=f"{', '.join(dets)} · {band_obj.rbw_6db_hz/1e3:g} kHz RBW",
        y_label="dBµV", y_unit="dBµV", series=series, summary=summary,
    )


@mcp.tool(
    title="CM/DM separation",
    description=(
        "Split the two LISN line measurements into common-mode and "
        "differential-mode. REQUIRES phase — magnitude-only spectra cannot be "
        "separated, and Hertz refuses rather than guessing."
    ),
    meta=UI_CURVES_META,
)
def separate_cm_dm(csv_text: str) -> CallToolResult:
    """Exact CM/DM separation from the two line signals.

    Args:
        csv_text: five numeric columns —
            frequency_Hz, L_mag_dBuV, L_phase_deg, N_mag_dBuV, N_phase_deg.
            Phase is mandatory: CM/DM separation is a vector operation, so a
            magnitude-only export is physically insufficient, not merely
            inconvenient.
    """
    f, lm, lp, nm, npd = _numeric_csv(
        csv_text, 5,
        "frequency_Hz, L_mag_dBuV, L_phase_deg, N_mag_dBuV, N_phase_deg")

    v_l = utils.vrms_from_dbuv(lm) * np.exp(1j * np.deg2rad(lp))
    v_n = utils.vrms_from_dbuv(nm) * np.exp(1j * np.deg2rad(npd))
    cm, dm = separate(v_l, v_n)
    cm_db = utils.dbuv_from_vrms(np.abs(cm))
    dm_db = utils.dbuv_from_vrms(np.abs(dm))

    series = [
        {"name": "Common mode", "color": "var(--s-2)",
         "points": [[float(a), float(b)] for a, b in zip(f, cm_db)]},
        {"name": "Differential mode", "color": "var(--s-1)",
         "points": [[float(a), float(b)] for a, b in zip(f, dm_db)]},
    ]
    i_cm, i_dm = int(np.argmax(cm_db)), int(np.argmax(dm_db))
    dominant = "common" if cm_db[i_cm] >= dm_db[i_dm] else "differential"
    summary = (
        f"CM peaks at {cm_db[i_cm]:.1f} dBµV ({f[i_cm]/1e6:.4g} MHz); "
        f"DM peaks at {dm_db[i_dm]:.1f} dBµV ({f[i_dm]/1e6:.4g} MHz). "
        f"{dominant.capitalize()} mode dominates -- attack it first: CM wants "
        f"choke and Y-caps, DM wants X-caps and leakage inductance."
    )
    return _curves_result(
        title="CM / DM separation", subtitle=f"{dominant} mode dominates",
        y_label="dBµV", y_unit="dBµV", series=series, summary=summary,
    )


@mcp.tool(
    title="Radiated estimate from CM current",
    description=(
        "Screen a common-mode cable current (dBuA) into an estimated radiated "
        "E-field and compare with the CISPR 32 radiated limit. Triage only: the "
        "screening model is worth about +/-20 dB."
    ),
    meta=UI_CURVES_META,
)
def estimate_radiated(
    csv_text: str,
    cable_length_m: float,
    distance_m: float = 10.0,
    emission_class: str = "B",
    level_column: int | None = None,
) -> CallToolResult:
    """Estimate radiated E-field from a CM current spectrum.

    Args:
        csv_text: frequency / CM current (dBuA) trace, e.g. a current-probe scan.
        cable_length_m: radiating cable length.
        distance_m: measurement distance (3 or 10 m).
    """
    freqs, dbua = _with_csv(csv_text, lambda p: read_spectrum_csv(
        p, level_column=level_column))
    e_field = radiated_efield_dbuvm(freqs, dbua, cable_length_m, distance_m)
    line = cispr32_radiated(emission_class.upper(), distance_m)

    covered, limit_levels = line.levels_where_covered(freqs)
    d_f, d_e = _decimate(freqs, np.asarray(e_field, dtype=float))
    series = [
        {"name": "Estimated E-field", "color": "var(--s-2)",
         "points": [[float(a), float(b)] for a, b in zip(d_f, d_e)]},
        _limit_series(line, f"CISPR 32 Class {emission_class.upper()} @ {distance_m:g} m"),
    ]

    if covered.any():
        margins = limit_levels[covered] - np.asarray(e_field)[covered]
        worst = float(np.min(margins))
        wf = float(freqs[covered][int(np.argmin(margins))])
        verdict = "OVER" if worst < 0 else "UNDER"
        head = (f"{verdict} the limit by {abs(worst):.1f} dB at "
                f"{wf/1e6:.4g} MHz (estimated).")
    else:
        head = ("No sample falls inside the radiated limit's frequency "
                "coverage -- nothing to compare.")

    summary = (
        f"{head} {cable_length_m:g} m cable at {distance_m:g} m.\n"
        "SCREENING ONLY: this is an Ott/Paul common-mode radiator "
        "approximation, good to roughly +/-20 dB. Use it to rank suspects and "
        "decide what to chamber-test, never as a pass/fail."
    )
    return _curves_result(
        title="Radiated estimate", subtitle=f"{cable_length_m:g} m cable @ {distance_m:g} m",
        y_label="dBµV/m", y_unit="dBµV/m", series=series, summary=summary,
        note="±20 dB screening model — triage, not a verdict.",
    )


@mcp.tool(
    title="Convert EMI levels and component values",
    description=(
        "Unit conversions that come up constantly in EMI work: dBuV <-> dBm "
        "<-> Vrms, and rounding a computed component value onto a real E-series "
        "part you can buy."
    ),
)
def convert_level(
    value: float,
    from_unit: str,
    to_unit: str,
    z0_ohm: float = 50.0,
) -> dict:
    """Convert between dBuV, dBm and Vrms.

    Args:
        from_unit / to_unit: 'dbuv', 'dbm', or 'vrms'.
        z0_ohm: reference impedance for dBm (50 ohm unless stated).
    """
    f, t = from_unit.lower(), to_unit.lower()
    valid = ("dbuv", "dbm", "vrms")
    if f not in valid or t not in valid:
        raise ValueError(f"units must be one of {valid}, got {from_unit!r}/{to_unit!r}")

    if f == "dbuv":
        dbuv = value
    elif f == "dbm":
        dbuv = float(utils.dbuv_from_dbm(value, z0_ohm))
    else:
        dbuv = float(utils.dbuv_from_vrms(value))

    if t == "dbuv":
        out = dbuv
    elif t == "vrms":
        out = float(utils.vrms_from_dbuv(dbuv))
    else:
        out = dbuv - float(utils.dbuv_from_dbm(0.0, z0_ohm))

    return {"value": out, "unit": to_unit,
            "as_dbuv": dbuv,
            "as_vrms": float(utils.vrms_from_dbuv(dbuv)),
            "z0_ohm": z0_ohm}


@mcp.tool(
    title="Round to an E-series value",
    description=(
        "Round a computed inductance or capacitance up or down onto a real "
        "E-series value. Filters are only as good as the parts you can actually "
        "order."
    ),
)
def round_to_series(
    value: float,
    direction: str = "up",
    series: list[float] | None = None,
) -> dict:
    """Round a component value onto an E-series.

    Args:
        value: the computed value, in base SI units (H, F, ...).
        direction: 'up' (safer for inductance) or 'down' (safer for the
            capacitance in a cutoff-frequency sense).
        series: per-decade series; defaults to E6.
    """
    s = tuple(series) if series else (1.0, 1.5, 2.2, 3.3, 4.7, 6.8)
    if direction not in ("up", "down"):
        raise ValueError("direction must be 'up' or 'down'")
    fn = utils.round_up_to_series if direction == "up" else utils.round_down_to_series
    rounded = float(fn(value, series=s))
    return {
        "input": value,
        "rounded": rounded,
        "direction": direction,
        "series": list(s),
        "error_percent": (rounded - value) / value * 100.0 if value else 0.0,
    }


# --- the MCP Apps UI resource ----------------------------------------------

@mcp.resource(
    UI_RESOURCE_URI,
    name="hertz-spectrum-widget",
    title="Hertz spectrum chart",
    mime_type=UI_RESOURCE_MIME,
)
def spectrum_widget() -> str:
    """The self-contained widget HTML rendered by MCP Apps hosts."""
    return _widget("spectrum.html")


@mcp.resource(
    UI_CURVES_URI,
    name="hertz-curves-widget",
    title="Hertz curve viewer (standalone)",
    mime_type=UI_RESOURCE_MIME,
)
def curves_widget() -> str:
    """Dependency-free chart. Kept as a fallback for anyone running the MCP
    server without a `web/` checkout alongside it."""
    return _widget("curves.html")


@mcp.tool(
    title="Probe WebAssembly support in this host",
    description=(
        "Report whether this chat host's widget sandbox permits WebAssembly. "
        "Answers an architecture question, not an EMC one: if WASM is blocked, "
        "engines must run server-side rather than inside the widget."
    ),
    meta=UI_WASMPROBE_META,
)
def wasm_probe() -> CallToolResult:
    """Ask the widget whether it can instantiate WebAssembly."""
    return CallToolResult(
        content=[TextContent(type="text", text=(
            "Probing the widget sandbox for WebAssembly support. The MCP Apps "
            "spec's recommended CSP (script-src 'self' 'unsafe-inline') grants "
            "neither 'wasm-unsafe-eval' nor 'unsafe-eval', so WASM is expected "
            "to be BLOCKED unless this host is more permissive. The widget "
            "reports the actual result."
        ))],
        structuredContent={"probe": "wasm"},
    )


@mcp.resource(
    UI_WASMPROBE_URI,
    name="hertz-wasm-probe",
    title="WASM capability probe",
    mime_type=UI_RESOURCE_MIME,
)
def wasmprobe_widget() -> str:
    """Reports whether the host sandbox allows WebAssembly."""
    return _widget("wasmprobe.html")


@mcp.resource(
    UI_CHART_URI,
    name="hertz-chart-widget",
    title="Hertz chart",
    mime_type=UI_RESOURCE_MIME,
)
def chart_widget() -> str:
    """The web app's own LogChart — same component as hertz.openconverters.com."""
    return _widget("chart.html")


@mcp.resource(
    UI_FILTER_URI,
    name="hertz-filter-widget",
    title="Hertz filter workbench",
    mime_type=UI_RESOURCE_MIME,
)
def filter_widget() -> str:
    """FilterSchematic + LogChart in switchable panes, as in the web workbench."""
    return _widget("filter.html")


def _widget(filename: str) -> str:
    bundle = Path(__file__).parent / "dist" / filename
    if not bundle.exists():
        raise FileNotFoundError(
            f"{bundle} missing -- build the widgets first: "
            f"cd mcp && npm install && npm run build"
        )
    return bundle.read_text(encoding="utf-8")


def build_app():
    """Starlette app with CORS.

    Browser-resident MCP hosts fetch /mcp from page JavaScript, so without
    these headers the connection dies at the preflight -- and the streamable
    transport additionally needs to READ `Mcp-Session-Id` off the response,
    which cross-origin JS cannot do unless the header is explicitly exposed.
    """
    from starlette.middleware.cors import CORSMiddleware

    app = mcp.streamable_http_app()
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],          # tighten to your host origins in production
        allow_methods=["GET", "POST", "DELETE", "OPTIONS"],
        allow_headers=["*"],
        expose_headers=["Mcp-Session-Id"],
    )
    return app


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(build_app(), host=mcp.settings.host, port=mcp.settings.port)
