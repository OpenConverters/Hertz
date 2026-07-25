#!/usr/bin/env python3
"""Export X2/Y2 safety-capacitor slices of the TAS capacitor catalog for the
Hertz filter designer.

    export_safety_caps.py /cache/kelvin/capacitor.ndjson /cache/kelvin/hertz-safety-caps.v1.json

CAS carries no dedicated safety-class field, so classification is by an
EXPLICIT X2/Y2 token in the series or part number ("MKP-X2", "WCAP-FTX2",
"...Y2..."), never inferred from technology — a film cap without the token may
well be X2-capable, but claiming a safety class the datasheet string does not
state would be invented data. The token must not be followed by a digit
(case codes like "12.5X20" are not safety classes).
"""

import json
import re
import sys

SAFETY_TOKEN = re.compile(r"(X2|Y2)(?![0-9])", re.IGNORECASE)


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


def classify(part_number, series):
    hits = {m.group(1).upper() for m in SAFETY_TOKEN.finditer(f"{series} {part_number}")}
    if hits == {"X2"}:
        return "X2"
    if hits == {"Y2"}:
        return "Y2"
    return None  # none or ambiguous ("X2/Y2" dual-rated strings stay out until modeled)


def main(source_path, output_path):
    parts = []
    skipped = 0
    with open(source_path) as source:
        for line in source:
            if "X2" not in line and "Y2" not in line and "x2" not in line and "y2" not in line:
                continue
            try:
                capacitor = json.loads(line)["capacitor"]
                info = capacitor["manufacturerInfo"]
                datasheet = info["datasheetInfo"]
                part = datasheet["part"]
                electrical = datasheet["electrical"]
            except (KeyError, json.JSONDecodeError):
                continue
            safety_class = classify(part.get("partNumber") or "", part.get("series") or "")
            if safety_class is None:
                continue
            capacitance = electrical.get("capacitance")
            if capacitance is None:
                skipped += 1
                continue
            try:
                capacitance_f = resolve_dimensional(capacitance)
            except ValueError:
                skipped += 1
                continue
            entry = {
                "mpn": part.get("partNumber"),
                "manufacturer": info.get("name"),
                "family": part.get("series") or "",
                "safetyClass": safety_class,
                "capacitanceF": capacitance_f,
                "ratedVoltageV": electrical.get("ratedVoltage"),
                "technology": part.get("technology") or "",
            }
            if entry["mpn"] and entry["manufacturer"]:
                parts.append(entry)

    parts.sort(key=lambda p: (p["safetyClass"], p["manufacturer"], p["capacitanceF"]))
    with open(output_path, "w") as output:
        json.dump({"version": 1, "count": len(parts), "parts": parts}, output)
    x2 = sum(1 for p in parts if p["safetyClass"] == "X2")
    print(f"wrote {len(parts)} safety caps ({x2} X2, {len(parts) - x2} Y2) to {output_path}; "
          f"{skipped} skipped without resolvable capacitance")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
