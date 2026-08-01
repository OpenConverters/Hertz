# The filter block: layout-aware filter design, characterized as one part

**Goal.** After Hertz selects real components, generate the filter's PCB
layout, analyze it in Faraday, and iterate layout + BOM together until the
filter meets its target **with the L and C of the components AND the
layout** — then hand the user one characterized block: board file, SPICE
model, S-parameters, BOM, prediction with stated error bands. Entirely
client-side (WASM); nothing is ever uploaded.

Why it's tractable: a line filter is not a general layout problem — it is a
canonical chain (terminals → X → CM choke → [X → choke] → Y pair → terminals
with a PE spine) in a handful of variants. That makes the generator a
**parametric template** with ~10–20 physical knobs, not an auto-router; and
every analysis piece (Faraday's loop-L ±15 % vs FastHenry, coupled-run
mutuals, cap-connection ESL; Hertz's ABCD/IL machinery; Kirchhoff's
in-process SPICE) already exists, is validated, and already runs in WASM.

**Follow-through.** Each stage is an ABT ticket; this file is the write-up,
the tickets are the backlog. A stage is DONE only when its acceptance
criteria are pinned in tests (Catch2 / Playwright) — the Faraday corpus
discipline: pins verified against physical sense, never silently re-pinned,
every capability with a stated error band, every silent limitation counted.

| Stage | ABT | Status |
|---|---|---|
| S1 LayoutGen: parametric KiCad board generator | #442 | **done** (KiCad 9: 0 DRC errors, 0 unconnected; Faraday-reviewed; 6 Catch2 pins) |
| S2 Layout parasitics → predicted IL | #443 | **done** (Grover strips ±1 % vs FastHenry; pair M ±1 % with the closing-vertical term, band ±50 % stated; numeric-Neumann pinned) |
| S3 Joint optimizer: layout params + BOM to target | #444 | **done** (deterministic ladder + stage escalation; meets/limited/refused all pinned) |
| S4 One-block deliverable: downloads + characterization | #445 | **done** (board/SPICE/s2p×2/BOM/report; LAYOUT & BLOCK pane; e2e) |

## S1 — LayoutGen (#442)

`Hertz::layout::generate_filter_board(design, packages, params)` →
`.kicad_pcb` text (KiCad v6 s-expression format, opened by KiCad 6–9).

- **Footprints are generated, not imported**: parametric THT builders (X2
  film box by pitch, Y2 disc, CM choke 4-pad, screw-terminal blocks,
  discharge R). Since Hertz emits both the footprint AND the netlist, pin
  consistency is by construction; the physical package of the *actual*
  ordered part must still be reviewed before fabrication, and the board says
  so on silk.
- **Default packages by value** (X2 pitch from capacitance, choke body from
  inductance) as clearly-labeled conservative defaults, overridable per part.
- **Electrical sizing built in**: trace widths from rated current
  (IPC-2221 external-layer approximation, 35 µm Cu, 30 K rise), L–N and
  line–PE spacing floors shaped by IEC 60664-1 (conservative defaults,
  overridable, **never a compliance claim** — the note on the board and in
  the docs says exactly that).
- Acceptance: KiCad 9 opens + passes DRC at declared clearances;
  `faraday_cli` imports and sees L/N/PE and every part; Catch2 pins the
  geometry (measured copper spacings ≥ floors, net integrity, 1- and
  2-stage, 2-wire first; 3/4-wire variants follow the same template later).

## S2 — Faraday in the loop (#443)

Run Faraday on the generated board (second WASM module on the page; CLI in
tests). Map extracted parasitics onto the filter model:

- input↔output mutual **M** as a bypass bridge across the whole cascade —
  the layout-limited IL floor ≈ 20·log₁₀(ωM/Z); this is the number that
  explains "80 dB filter delivers 40 dB".
- X/Y-cap **connection ESL** (trace + via) shifting each capacitor's SRF.
- shared-return **common impedance** between stages.

Every derived curve carries its band. FastHenry spot-check (recorded in
#443): the Y-stub strip partial lands within 1 % (4.26 vs 4.30 nH); the
pair bypass M required the loops' CLOSING-VERTICAL term — horizontal-only
measured 2.2× low (1.26 vs 2.74 nH), with it the closed form is within 1 %
of the solver for this template. The split is configuration-dependent, so
the stated absolute band is ±50 % (floors ±4 dB); rankings within one
template family are far tighter. The filament mutual is additionally pinned
against a numeric Neumann double integral in Catch2.

ARCHITECTURE NOTE (decided in S2): the optimizer's inner loop uses the
ANALYTIC parasitics from the generator's own geometry summary (fast, exact
to the template); Faraday remains the independent reviewer of the emitted
text — the pane links every downloaded board to faraday.openconverters.com
for the EMC review of exactly those bytes. Faraday's dangling-stub rule
gained T-junction awareness from reviewing the first generated board.

## S3 — The joint iteration (#444)

Coordinate descent over template parameters against the required-attenuation
target; if the layout-limited floor cannot reach spec at any setting, the
loop re-enters `design_line_filter` with the derated target (larger choke /
second stage) and regenerates. Board analysis is ~tens of ms in WASM, so
hundreds of iterations are interactive. Determinism rule from Faraday: no
wall-clock, no iteration-order dependence — seeded, sorted, quantized.

## S4 — One block, several formats (#445)

Downloads: `.kicad_pcb` (stamped machine-generated / review before fab),
SPICE `.subckt` **including layout parasitics** (verifiable in Kirchhoff
against CISPR 17 worst-case terminations), `.s2p` per mode from the ABCD
chain, BOM CSV, IL-report JSON with bands. Web: the "filter block" pane —
schematic + board + predicted vs required IL + the parasitic budget. Gerber
emission is deliberately out of scope: KiCad is the fabrication boundary.

## The source chain (built 2026-08-01, after the four stages)

The Faraday→Hertz predictive bridge is now LIVE: Faraday's emissions panel
computes a pre-hardware CONDUCTED estimate (the exact trapezoid comb into
the input-cap branch for DM, an ASSUMED C_stray for CM — bands ±10/±15 dB
stated in the payload) and ships the per-mode dBuV spectra to
hertz.openconverters.com in the URL FRAGMENT (never reaches a server).
Hertz judges them against CISPR 32 B quasi-peak like any measured scan,
seeds the binding sets with a visible "from a FARADAY BOARD PREDICTION"
provenance note, tells the user outright when the prediction already
passes, and the LAYOUT & BLOCK pane closes the loop with a generated PCB.
An unmet target is DIAGNOSED: catalog-limited (bind larger parts) vs
layout-limited (spacing/shielding) — the live demo chain surfaced a 0.1 dB
catalog miss mislabeled as layout, which forced the distinction into the
engine. Proven end-to-end on production with the MPPT demo board.

## Out of scope (recorded, not forgotten)

- 3-phase templates (S1 covers the 2-wire pair first; the template
  generalizes, `x_capacitor_dm_factor` already does).
- OMFEM choke digital twin (leakage L, EPC, wideband Z_CM) — composes with
  this plan, does not gate it.
- Physical build validation (Tier 3) — explicitly deferred by the user.
