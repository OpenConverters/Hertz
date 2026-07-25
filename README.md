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
| `hertz.filter_design` | Single-phase line-filter synthesis per Würth Elektronik's public application note ANP015: 1- and 2-stage CM+DM sizing, component rounding onto explicit candidate lists, Y-cap leakage current, X-cap discharge resistor. Validated against the app note's worked example. |
| `hertz.traces` | Spectrum-analyzer CSV ingestion with unit detection; ambiguous units raise instead of guessing. |

## Install & test

```bash
pip install -e ".[dev]"
pytest
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
