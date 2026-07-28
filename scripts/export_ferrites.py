#!/usr/bin/env python3
"""Export a compact ferrite-bead / cable-suppressor slice of the TAS magnetic
catalog for Hertz's radiated cable-mitigation picker.

    export_ferrites.py /cache/kelvin/magnetic.ndjson /cache/kelvin/hertz-ferrites.v1.json \
        [/cache/kelvin/hertz-ferrites-curves.v1.json]

A ferrite bead / clip-on core is, to first order, a pure SERIES common-mode
impedance Z(f) on a cable — exactly what select_cable_mitigation consumes.
Unlike a choke it is specified by its impedance CURVE, not an inductance, so a
part is a candidate ONLY when it carries a resolvable measured |Z|(f) curve;
parts without one are skipped (counted, reported) — a bead with no curve cannot
be selected, and inventing one would poison the pick.

The main slice keeps what the picker and the parts table show: MPN, maker,
family, the headline |Z| at 100 MHz, the measured peak (|Z|, f), rated current
and DCR. The curve itself goes to the side file keyed by MPN, striding the
MEASURED points to <= 64 — never resampled or extrapolated.

These parts (subtype `chipBead`) are magnitude-only in the corpus, so every
curve's phase is RECONSTRUCTED with the Bode gain-phase relation
(phase ~ (pi/2) * d ln|Z| / d ln f — exact for minimum-phase immittances, which
a passive one-port bead is up to its resonance null); the reconstruction is the
same one validated to 0.03 dB median / 0.52 dB p90 against Murata's measured
phase in the CMC export. Every curve here is therefore marked "rec": true and
the GUI labels the pick as reconstructed-phase.

NOTE ON PART CLASS: `chipBead` parts are SMD suppression beads (single-pass
series elements), not clamp-on cable cores you thread turns through. The series
common-mode-impedance model is valid for both, but a bead is physically a
turns=1 part. True clamp-on/cable cores are a distinct family that is not yet a
subtype in the TAS catalog; bringing them in is an upstream data/schema task.
"""

import json
import math
import sys

MANUFACTURER_CANONICAL = {
    "ABRACON": "Abracon",
    "Murata Electronics": "Murata",
    "Pulse Electronics": "PULSE",
}

MAX_CURVE_POINTS = 64
Z_HEADLINE_HZ = 100e6  # beads are conventionally rated by |Z| at 100 MHz


def resolve_dimensional(value):
    """PEAS resolve_dimensional_values rule: nominal -> (min+max)/2 -> max -> min."""
    if isinstance(value, (int, float)):
        return float(value)
    if not isinstance(value, dict):
        raise ValueError(f"unresolvable dimensional value: {value!r}")
    if value.get("nominal") is not None:
        return float(value["nominal"])
    if value.get("minimum") is not None and value.get("maximum") is not None:
        return (float(value["minimum"]) + float(value["maximum"])) / 2.0
    if value.get("maximum") is not None:
        return float(value["maximum"])
    if value.get("minimum") is not None:
        return float(value["minimum"])
    raise ValueError(f"unresolvable dimensional value: {value!r}")


def plausible_dcr(dcr_ohm, rated_a):
    """None unless the value can be a real winding DC resistance. Beads carry a
    small DCR (mOhm..a few Ohm); a stored Z@100MHz spec masquerading as DCR
    (hundreds of Ohm) or a 0.0 sentinel poisons the thermal read, so drop it."""
    if dcr_ohm is None or dcr_ohm <= 0.0 or dcr_ohm > 50.0:
        return None
    if rated_a is not None and dcr_ohm * rated_a * rated_a > 5.0:
        return None
    return dcr_ohm


def bode_phase(freqs, mags):
    """Minimum-phase estimate from |Z| via the Bode gain-phase relation:
    phase ~ (pi/2) * d ln|Z| / d ln f, central differences smoothed over 7
    points. Only ever applied to curves carrying no measured phase."""
    lnf = [math.log(f) for f in freqs]
    lnz = [math.log(max(m, 1e-9)) for m in mags]
    n = len(lnf)
    slope = []
    for i in range(n):
        a, b = max(0, i - 1), min(n - 1, i + 1)
        slope.append((lnz[b] - lnz[a]) / (lnf[b] - lnf[a]) if lnf[b] != lnf[a] else 0.0)
    half = 3
    smoothed = [sum(slope[max(0, i - half):i + half + 1]) / len(slope[max(0, i - half):i + half + 1])
                for i in range(n)]
    return [(math.pi / 2) * s for s in smoothed]


def measured_magnitudes(points):
    """Sorted [(f, |Z|)] for a 2-terminal bead (winding is null in the corpus).
    Skips malformed points; never invents a value."""
    rows = []
    for point in points or []:
        # bead points carry no winding tag (None); a tagged multi-winding part
        # is not a bead and would be miscategorised, so keep only untagged.
        if point.get("winding") is not None:
            continue
        f = point.get("frequency")
        z = point.get("impedance") or {}
        mag = z.get("magnitude")
        if not all(isinstance(x, (int, float)) for x in (f, mag)) or f <= 0 or mag < 0:
            continue
        rows.append((float(f), float(mag)))
    rows.sort()
    return rows


def interp_loglog(rows, f_target):
    """|Z| at f_target by log-log interpolation of measured (f, |Z|). None when
    f_target is outside the measured span — no extrapolation of a headline spec."""
    if len(rows) < 2 or f_target < rows[0][0] or f_target > rows[-1][0]:
        return None
    for j in range(1, len(rows)):
        if rows[j][0] >= f_target:
            (f0, z0), (f1, z1) = rows[j - 1], rows[j]
            if z0 <= 0 or z1 <= 0 or f1 == f0:
                return z1
            t = (math.log(f_target) - math.log(f0)) / (math.log(f1) - math.log(f0))
            return math.exp(math.log(z0) + t * (math.log(z1) - math.log(z0)))
    return rows[-1][1]


def build_curve(rows):
    """Strided (<= 64) complex curve with Bode-reconstructed phase, or None."""
    if len(rows) < 2:
        return None
    stride = max(1, -(-len(rows) // MAX_CURVE_POINTS))
    picked = rows[::stride]
    if picked[-1] != rows[-1]:
        picked.append(rows[-1])
    freqs = [r[0] for r in picked]
    mags = [r[1] for r in picked]
    phases = bode_phase(freqs, mags)
    return {"f": freqs,
            "re": [m * math.cos(p) for m, p in zip(mags, phases)],
            "im": [m * math.sin(p) for m, p in zip(mags, phases)],
            "rec": True}


def main(source_path, output_path, curves_path=None):
    parts = []
    curves = {}
    skipped_no_curve = 0
    with open(source_path) as source:
        for line in source:
            try:
                magnetic = json.loads(line)["magnetic"]
                info = magnetic["manufacturerInfo"]
                electrical = info["datasheetInfo"]["electrical"][0]
            except (KeyError, IndexError, json.JSONDecodeError):
                continue
            subtype = str(electrical.get("subtype", "")).lower().replace("_", "").replace("-", "")
            # chipBead = SMD suppression bead (single-pass series element);
            # cableCore = clamp-on / cable ferrite core the cable is threaded
            # through. Both are a 1-port series impedance the picker consumes; the
            # `kind` tag lets the cable-mitigation UI prefer real clamp-on cores.
            if subtype not in ("chipbead", "cablecore"):
                continue
            rows = measured_magnitudes(electrical.get("impedancePoints"))
            curve = build_curve(rows)
            if curve is None:
                skipped_no_curve += 1
                continue
            rated = electrical.get("ratedCurrents") or []
            rated_a = max(rated) if rated else None
            dcr = electrical.get("dcResistance")
            part = {
                "mpn": info.get("reference"),
                "manufacturer": MANUFACTURER_CANONICAL.get(info.get("name"), info.get("name")),
                "family": info.get("family") or "",
                "kind": "cableCore" if subtype == "cablecore" else "bead",
                "zAt100MHzOhm": interp_loglog(rows, Z_HEADLINE_HZ),
                "zPeakOhm": max(m for _, m in rows),
                "fPeakHz": max(rows, key=lambda r: r[1])[0],
                "ratedCurrentA": rated_a,
                "dcrOhm": plausible_dcr(resolve_dimensional(dcr) if dcr is not None else None, rated_a),
                # cable-core selection metadata (null for beads)
                "mountingForm": electrical.get("mountingForm"),
                "cableMaxM": electrical.get("maximumCableOuterDiameter"),
            }
            if part["mpn"] and part["manufacturer"]:
                parts.append(part)
                if curves_path is not None:
                    curves[part["mpn"]] = curve

    parts.sort(key=lambda p: (p["manufacturer"],
                              p["zAt100MHzOhm"] is None, p["zAt100MHzOhm"] or 0.0))
    with open(output_path, "w") as output:
        json.dump({"version": 1, "count": len(parts), "parts": parts}, output)
    kinds = {}
    for p in parts:
        kinds[p["kind"]] = kinds.get(p["kind"], 0) + 1
    print(f"wrote {len(parts)} ferrite parts ({kinds}) to {output_path} "
          f"({skipped_no_curve} skipped without a resolvable impedance curve)")
    if curves_path is not None:
        with open(curves_path, "w") as handle:
            json.dump({"version": 1, "count": len(curves), "curves": curves}, handle)
        print(f"wrote measured curves for {len(curves)} parts to {curves_path}")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
