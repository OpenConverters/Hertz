#!/usr/bin/env python3
"""Export a compact common-mode-choke slice of the TAS magnetic catalog for the
Hertz filter designer.

    export_cmc_catalog.py /cache/kelvin/magnetic.ndjson /cache/kelvin/hertz-cmc.v1.json \
        [/cache/kelvin/hertz-cmc-curves.v1.json]

Rows keep only what candidate selection needs: part number, manufacturer,
family, nominal inductance, rated current, DCR. Parts without a resolvable
inductance are skipped (counted, reported) — a choke with no L cannot be a
candidate, and inventing one would poison the design.

With a third argument, measured impedance curves (impedancePoints with
magnitude AND phase, per winding: common/differential) are written to a side
file keyed by MPN, downsampled by striding the MEASURED points to <= 64 per
mode — never resampled or extrapolated. Parts carrying a curve get
hasMeasuredCmCurve / hasMeasuredDmCurve flags in the main slice.
"""

import json
import sys


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


MAX_CURVE_POINTS = 64


def extract_curve(points, winding):
    rows = []
    for point in points or []:
        if point.get("winding") != winding:
            continue
        f = point.get("frequency")
        z = point.get("impedance") or {}
        mag, phase = z.get("magnitude"), z.get("phase")
        if not all(isinstance(x, (int, float)) for x in (f, mag, phase)) or f <= 0 or mag < 0:
            continue
        rows.append((float(f), float(mag), float(phase)))
    rows.sort()
    if len(rows) < 2:
        return None
    stride = max(1, -(-len(rows) // MAX_CURVE_POINTS))
    picked = rows[::stride]
    if picked[-1] != rows[-1]:
        picked.append(rows[-1])
    import math
    return {"f": [r[0] for r in picked],
            "re": [r[1] * math.cos(r[2]) for r in picked],
            "im": [r[1] * math.sin(r[2]) for r in picked]}


def main(source_path, output_path, curves_path=None):
    parts = []
    curves = {}
    skipped_no_inductance = 0
    with open(source_path) as source:
        for line in source:
            try:
                magnetic = json.loads(line)["magnetic"]
                info = magnetic["manufacturerInfo"]
                electrical = info["datasheetInfo"]["electrical"][0]
            except (KeyError, IndexError, json.JSONDecodeError):
                continue
            subtype = str(electrical.get("subtype", "")).lower().replace("_", "").replace("-", "")
            if subtype != "commonmodechoke":
                continue
            inductance = electrical.get("inductance")
            if inductance is None:
                skipped_no_inductance += 1
                continue
            try:
                inductance_h = resolve_dimensional(inductance)
            except ValueError:
                skipped_no_inductance += 1
                continue
            rated = electrical.get("ratedCurrents") or []
            dcr = electrical.get("dcResistance")
            curve_cm = extract_curve(electrical.get("impedancePoints"), "common") if curves_path else None
            curve_dm = extract_curve(electrical.get("impedancePoints"), "differential") if curves_path else None
            part = {
                "mpn": info.get("reference"),
                "manufacturer": info.get("name"),
                "family": info.get("family") or "",
                "inductanceH": inductance_h,
                "ratedCurrentA": max(rated) if rated else None,
                "dcrOhm": resolve_dimensional(dcr) if dcr is not None else None,
            }
            if part["mpn"] and part["manufacturer"]:
                if curve_cm or curve_dm:
                    part["hasMeasuredCmCurve"] = curve_cm is not None
                    part["hasMeasuredDmCurve"] = curve_dm is not None
                    curves[part["mpn"]] = {k: v for k, v in
                                           (("cm", curve_cm), ("dm", curve_dm)) if v}
                parts.append(part)

    parts.sort(key=lambda p: (p["manufacturer"], p["inductanceH"]))
    with open(output_path, "w") as output:
        json.dump({"version": 1, "count": len(parts), "parts": parts}, output)
    print(f"wrote {len(parts)} common-mode chokes to {output_path} "
          f"({skipped_no_inductance} skipped without resolvable inductance)")
    if curves_path is not None:
        with open(curves_path, "w") as handle:
            json.dump({"version": 1, "count": len(curves), "curves": curves}, handle)
        print(f"wrote measured curves for {len(curves)} parts to {curves_path}")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
