# Hertz

Open-source **conducted-EMI analysis and EMI-filter design** toolkit for power
electronics. Sibling of [Kirchhoff](../Kirchhoff) (in-process SPICE) and
[Kelvin](../Kelvin) (parts librarian) in the OpenConverters ecosystem.

Hertz turns the mathematics of the conducted-emissions world — LISNs, CISPR
receiver detectors, limit lines, CM/DM separation, line-filter synthesis — into
tested, reusable code. It is vendor-neutral by design: synthesis works over any
manufacturer's component catalog supplied as candidate lists.

> **Pre-compliance estimates only.** Nothing here replaces accredited chamber
> testing, and no output of this library is a compliance statement.

## Modules (v0.1)

| Module | What it does |
|---|---|
| `hertz.limits` | Limit lines (CISPR 32 Class A/B mains, CISPR 25 classes 1–5 conducted) with log-frequency interpolation. Frequencies outside a line's coverage raise — CISPR 25 defines limits only inside protected bands, and "no limit here" is information. |
| `hertz.lisn` | 50 µH (CISPR 16) and 5 µH (CISPR 25) LISN models: EUT-side impedance vs frequency and SPICE subcircuit export. |
| `hertz.detector` | CISPR 16-1-1 measuring-receiver emulation on sampled data: Gaussian-window STFT envelope + quasi-peak charge/discharge detector + critically damped meter (Krug & Russer, IEEE TEMC 2005). Bands A–D constants included. |
| `hertz.separation` | Exact CM/DM separation from the two LISN line signals. Deliberately refuses magnitude-only spectra (phase is required — that is physics, not a missing feature). |
| `hertz.filter_design` | Line-filter synthesis per Würth Elektronik's public application note ANP015, generalized over the line count: single-phase L/N, DC supply pair, 3-phase 3-wire (delta X, each pair sees 1.5·C) and 3-phase + neutral (star X). 1- and 2-stage CM+DM sizing, component rounding onto explicit candidate lists, Y-cap leakage current at phase-to-earth voltage, X-cap discharge resistor. Validated against the app note's worked example. |
| `hertz.traces` | Spectrum-analyzer CSV ingestion with unit detection; ambiguous units raise instead of guessing. |

## Architecture

Two implementations of the same engine, sharing one set of golden test vectors:

- **`cpp/` — the product core** (C++20, header-only): compiles natively, to
  **WASM** (browser tools run entirely client-side — no simulation servers), and
  to Python via pybind11 (`PyHertz`). Reuses the OpenMagnetics **MKF**
  library (its radix-2 FFT today; its wideband choke impedance model next), so
  build MKF first and point `HERTZ_MKF_ROOT` at the checkout.
- **`src/hertz/` — the Python reference** (numpy): the readable spec and
  cross-validation implementation; every C++ behavior is asserted against the
  same numbers here.
- **`src/hertz_cpp/` — the same API on the C++ engine**: a drop-in replacement
  for `hertz` that calls `PyHertz`. It adapts shapes only (numpy in/out, tuple
  returns, path-taking CSV readers) and reimplements no formula, so the
  reference suite can be pointed straight at the C++ engine.

CSV parsing is string-based in the C++ core by design: the host (browser `File`
API, Python `open()`) reads the file and passes its content — the WASM-native
pattern.

## Install & test

```bash
# Python reference
pip install -e ".[dev]"
pytest

# C++ core (Catch2 binary, run it directly — no ctest)
cmake -S cpp -B cpp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C cpp/build -j4
./cpp/build/test_hertz

# The SAME 92 tests against the C++ engine through PyHertz (built by the
# command above; -DHERTZ_BUILD_PYBIND=OFF skips it). The tests are the golden
# vectors shared by both implementations, so a failure here is a real
# disagreement between the two engines — never something to silence.
pytest --backend=cpp
```

`PyHertz` mirrors `src/hertz/*.py` name for name, plus the surface the Python
reference does not carry: SPICE deck emission (`filter_spice_deck`,
`lisn_reference_deck`, `deck_abcd_il`), the radiated CM-attenuation target, the
cable-ferrite picker, and `limit_polyline_runs`.

## Web GUI (`web/`)

The full engine is exposed as a browser instrument — Vue 3 + Vite + the WASM
engine, zero server compute — deployed at **hertz.openconverters.com**. Two
destinations, because the workflow is measure-then-design: **Spectrum** (scan vs
limit verdict, worst offenders, required attenuation, handoff to the designer)
and **Filter** — the ANP015 designer with any manufacturer's candidate lists +
SPICE export, laid out as a workbench: an input rail on the left, two
independently switchable output panes on the right.

Everything else is a pane of that bench rather than a screen of its own, because
each one is an input to a filter design or a reading of one, and answers little
on its own: **MEASURE · SCOPE CAPTURE** (CISPR 16-1-1 peak/quasi-peak/average
emulation of an uploaded waveform, which hands per-mode CM/DM targets straight to
the rail), **MEASURE · CM PROBE** (radiated pre-scan from a CM-current capture,
plus the cable-ferrite mitigation picker), **TEST SETUP (LISN)** (impedance
explorer + SPICE model), and the design outputs — schematic, catalog parts, BOM,
insertion loss, sizing & safety, SPICE netlist, predicted result.

```bash
scripts/build_wasm.sh        # engine → web/public/ (needs emsdk + WebLibMKF)
cd web && npm install && npm run dev    # develop
npm run build && npm test               # dist/ + Playwright e2e (headless)
```

## Roadmap

- Harmonic-comb detection / switching-frequency estimation from measured spectra
- Filter synthesis driven by Kelvin candidate lists and verified in-circuit with
  Kirchhoff (real source/LISN impedances instead of 50 Ω, CISPR 17 worst-case
  0.1 Ω/100 Ω terminations)
- Component model pipeline: measured/FEM impedance → vector-fitted SPICE
  subcircuits (scikit-rf) → nonlinear saturation cores (Verilog-A/OSDI)
- Middlebrook input-filter stability check and damping design
- CISPR band A LISN branch; MIL-STD-461 / DO-160 limit packs (after verification
  against the standards)

## License

MIT — see [LICENSE](LICENSE).
