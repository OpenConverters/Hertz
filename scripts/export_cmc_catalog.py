#!/usr/bin/env python3
"""Export a compact common-mode-choke slice of the TAS magnetic catalog for the
Hertz filter designer.

    export_cmc_catalog.py /cache/kelvin/magnetic.ndjson /cache/kelvin/hertz-cmc.v1.json

Rows keep only what candidate selection needs: part number, manufacturer,
family, nominal inductance, rated current, DCR. Parts without a resolvable
inductance are skipped (counted, reported) — a choke with no L cannot be a
candidate, and inventing one would poison the design.
"""

import json
import sys

# ABT #279: the Würth .mdb import tags CMC families as subtype "inductor";
# until that is fixed upstream, these family prefixes are included explicitly.
WE_CMC_FAMILY_PREFIXES = ("WE-CMB", "WE-CNSW", "WE-SL", "WE-LF", "WE-FC")


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


def main(source_path, output_path):
    parts = []
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
            family_upper = str(info.get("family") or "").upper()
            is_cmc = subtype == "commonmodechoke" or any(
                family_upper.startswith(prefix) for prefix in WE_CMC_FAMILY_PREFIXES)
            if not is_cmc:
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
            part = {
                "mpn": info.get("reference"),
                "manufacturer": info.get("name"),
                "family": info.get("family") or "",
                "inductanceH": inductance_h,
                "ratedCurrentA": max(rated) if rated else None,
                "dcrOhm": resolve_dimensional(dcr) if dcr is not None else None,
            }
            if part["mpn"] and part["manufacturer"]:
                parts.append(part)

    parts.sort(key=lambda p: (p["manufacturer"], p["inductanceH"]))
    with open(output_path, "w") as output:
        json.dump({"version": 1, "count": len(parts), "parts": parts}, output)
    print(f"wrote {len(parts)} common-mode chokes to {output_path} "
          f"({skipped_no_inductance} skipped without resolvable inductance)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
