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

# Slice-level canonicalization of manufacturer spellings (duplicates found in
# review; upstream data fix tracked in ABT). Majority spelling wins.
def plausible_dcr(dcr_ohm, rated_a):
    """None unless the value can be a real winding DC resistance. Review found
    Z@100MHz specs stored as dcResistance (3000 ohm 'DCR' on a 1.5 A part) and
    0.0 sentinels; showing either as DCR poisons thermal judgement."""
    if dcr_ohm is None or dcr_ohm <= 0.0 or dcr_ohm > 50.0:
        return None
    if rated_a is not None and dcr_ohm * rated_a * rated_a > 5.0:
        return None
    return dcr_ohm


# Highest genuine CMC in the corpus is a 110 A busbar part; anything beyond
# this is a data error (found live: iNRCORE R82xx rows carrying the PL82xx
# twins' ratings x100 — a 14 A choke listed as 1400 A would be recommended
# for a >=100 A design and saturate at the operating point). Quarantined and
# reported, never shipped; upstream fix tracked in ABT.
IMPLAUSIBLE_RATED_A = 200.0


MANUFACTURER_CANONICAL = {
    "ABRACON": "Abracon",
    "Murata Electronics": "Murata",
    "Pulse Electronics": "PULSE",
}


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
    quarantined_rated = []
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
            # SILENT-method support: a part with no resolvable inductance is still a
            # candidate when it carries a measured CM impedance curve — it is then
            # selectable BY the curve (inductanceH stays null, never invented).
            inductance = electrical.get("inductance")
            inductance_h = None
            if inductance is not None:
                try:
                    inductance_h = resolve_dimensional(inductance)
                except ValueError:
                    inductance_h = None
            rated = electrical.get("ratedCurrents") or []
            if rated and max(rated) > IMPLAUSIBLE_RATED_A:
                quarantined_rated.append(f"{info.get('reference')} ({max(rated):g} A)")
                continue
            # CMC rows carry per-winding dcResistances[]; fall back to the
            # legacy singular for older rows.
            dcr_list = electrical.get("dcResistances")
            dcr = (dcr_list[0] if isinstance(dcr_list, list) and dcr_list
                   else electrical.get("dcResistance"))
            curve_cm = extract_curve(electrical.get("impedancePoints"), "common") if curves_path else None
            curve_dm = extract_curve(electrical.get("impedancePoints"), "differential") if curves_path else None
            if inductance_h is None and curve_cm is None:
                skipped_no_inductance += 1
                continue
            part = {
                "mpn": info.get("reference"),
                "manufacturer": MANUFACTURER_CANONICAL.get(info.get("name"), info.get("name")),
                "family": info.get("family") or "",
                "inductanceH": inductance_h,
                "ratedCurrentA": max(rated) if rated else None,
                "dcrOhm": plausible_dcr(resolve_dimensional(dcr) if dcr is not None else None,
                                        max(rated) if rated else None),
                "ratedVoltageAcV": electrical.get("ratedVoltageAC"),
                "ratedVoltageDcV": electrical.get("ratedVoltageDC"),
            }
            if part["mpn"] and part["manufacturer"]:
                if curve_cm or curve_dm:
                    part["hasMeasuredCmCurve"] = curve_cm is not None
                    part["hasMeasuredDmCurve"] = curve_dm is not None
                    curves[part["mpn"]] = {k: v for k, v in
                                           (("cm", curve_cm), ("dm", curve_dm)) if v}
                parts.append(part)

    parts.sort(key=lambda p: (p["manufacturer"], p["inductanceH"] is None, p["inductanceH"] or 0.0))
    with open(output_path, "w") as output:
        json.dump({"version": 1, "count": len(parts), "parts": parts}, output)
    print(f"wrote {len(parts)} common-mode chokes to {output_path} "
          f"({skipped_no_inductance} skipped without resolvable inductance)")
    if quarantined_rated:
        print(f"QUARANTINED {len(quarantined_rated)} parts with implausible rated current "
              f"(> {IMPLAUSIBLE_RATED_A:g} A): {', '.join(quarantined_rated)}")
    if curves_path is not None:
        with open(curves_path, "w") as handle:
            json.dump({"version": 1, "count": len(curves), "curves": curves}, handle)
        print(f"wrote measured curves for {len(curves)} parts to {curves_path}")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    main(*sys.argv[1:])
