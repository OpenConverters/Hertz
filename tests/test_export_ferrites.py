"""The ferrite export must account for every part it sets aside.

The picker's answer is only interpretable as "best of a stated pool", so a part
that never reaches the browser has to be counted and reported. Two exclusions
mean different things and must not merge into one tally:

  * a SINGLE-POINT part (exactly one measured |Z|) is real and fully specified —
    we just cannot place it against a flagged band, because one point fixes no
    slope. These are recoverable by an upstream curve ingest.
  * a NO-DATA part carries no usable impedance point at all.

These tests also pin the thing that must NOT happen: a single point never
becomes a curve. Inventing the slope would make the pick unfalsifiable.
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))

import export_ferrites   # noqa: E402


def _magnetic(reference, subtype, points, status="production"):
    return {"magnetic": {"manufacturerInfo": {
        "reference": reference, "name": "Fixture", "family": "FIX", "status": status,
        "datasheetInfo": {"electrical": [{
            "subtype": subtype,
            "impedancePoints": [{"frequency": f, "impedance": {"magnitude": z}} for f, z in points],
            "ratedCurrents": [1.0], "dcResistance": 0.05,
            "mountingForm": "snapOn", "maximumCableOuterDiameter": 0.012,
        }]}}}}


def _run(tmp_path, records):
    source = tmp_path / "magnetic.ndjson"
    source.write_text("\n".join(json.dumps(r) for r in records) + "\n")
    main_out, curves_out = tmp_path / "main.json", tmp_path / "curves.json"
    export_ferrites.main(str(source), str(main_out), str(curves_out))
    return json.loads(main_out.read_text()), json.loads(curves_out.read_text())


def test_single_point_parts_are_counted_apart_from_no_data_parts(tmp_path):
    main, curves = _run(tmp_path, [
        _magnetic("CURVE-1", "cableCore", [(10e6, 100), (100e6, 300), (1e9, 250)]),
        _magnetic("ONEPOINT-1", "cableCore", [(100e6, 220)]),
        _magnetic("ONEPOINT-2", "cableCore", [(100e6, 180)]),
        _magnetic("NODATA-1", "cableCore", []),
    ])
    # only the curve-carrying part is a candidate
    assert [p["mpn"] for p in main["parts"]] == ["CURVE-1"]
    assert set(curves["curves"]) == {"CURVE-1"}
    # ...and the rest are accounted for, by REASON, in the file the app reads
    assert main["excluded"]["cableCore"] == {"singlePointOnly": 2, "noImpedanceData": 1}


def test_a_single_point_never_becomes_a_curve(tmp_path):
    """One measured point defines no d ln|Z|/d ln f, so no curve may be built
    from it — anything else is an invented slope wearing a measurement's name."""
    main, curves = _run(tmp_path, [_magnetic("ONEPOINT", "bead", [(100e6, 600)])])
    assert main["parts"] == []
    assert curves["curves"] == {}
    assert export_ferrites.build_curve([(100e6, 600.0)]) is None


def test_exclusions_are_tallied_per_kind(tmp_path):
    """The picker draws from cable cores when it has them and beads otherwise, so
    a bead exclusion must not be reported against a cable-core pick."""
    main, _ = _run(tmp_path, [
        _magnetic("CORE-OK", "cableCore", [(10e6, 100), (100e6, 300)]),
        _magnetic("CORE-1PT", "cableCore", [(100e6, 220)]),
        _magnetic("BEAD-1PT", "chipBead", [(100e6, 600)]),
        _magnetic("BEAD-NONE", "chipBead", []),
    ])
    assert main["excluded"]["cableCore"] == {"singlePointOnly": 1, "noImpedanceData": 0}
    assert main["excluded"]["bead"] == {"singlePointOnly": 1, "noImpedanceData": 1}


def test_obsolete_parts_are_not_counted_as_an_impedance_gap(tmp_path):
    """An obsolete part is excluded for being unbuyable, upstream of the curve
    check — folding it into the single-point tally would overstate the data gap
    and send someone chasing curves for parts nobody can order."""
    main, _ = _run(tmp_path, [
        _magnetic("OLD-1PT", "cableCore", [(100e6, 220)], status="obsolete"),
        _magnetic("CORE-OK", "cableCore", [(10e6, 100), (100e6, 300)]),
    ])
    assert main["excluded"]["cableCore"] == {"singlePointOnly": 0, "noImpedanceData": 0}
    assert [p["mpn"] for p in main["parts"]] == ["CORE-OK"]
