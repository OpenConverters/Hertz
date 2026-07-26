<script setup>
// ANP015 line-filter designer, laid out as a Kirchhoff-style workbench: a
// compact input rail on the left (requirement → components → grid & safety),
// a verdict strip plus TWO independently switchable output panes on the right
// (schematic, catalog parts, BOM, insertion loss, sizing tables, netlist).
// The whole bench fits the viewport — panes scroll inside themselves.
import { computed, onMounted, ref, watch } from 'vue'
import LogChart from './LogChart.vue'
import FilterSchematic from './FilterSchematic.vue'
import LisnView from './LisnView.vue'
import { api } from '../engine.js'
import { buildFilterCias, filterComponents } from '../ciasFilter.js'
import { store } from '../store.js'
import { fmtHz, fmtDb, fmtSi } from '../format.js'

const fSwKhz = ref(300)
const aReqCm = ref(40)
const aReqDm = ref(40)
const cYnF = ref('auto')            // 'auto' = largest standard Y within the touch budget
const stages = ref(1)
const dmMode = ref('impedance')     // 'impedance' | 'inductance' | 'points'
const dmImpedanceOhm = ref(92)
const dmImpedanceMhz = ref(1)
const lDmUh = ref(14.6)
const dmPointsText = ref('')        // "MHz, Ohm" per line — fitted in the inductive region
const dmPointsNote = ref('')
const lCandidatesMh = ref('0.47, 0.68, 1, 1.5, 2.2, 3.3, 4.7, 6.8, 10')
const cxCandidatesUf = ref('0.1, 0.15, 0.22, 0.33, 0.47, 0.68, 1, 1.5, 2.2, 3.3')
const cxSource = ref('manual')      // 'manual' | 'catalog' (X2 safety caps)
const cxMfr = ref('')
const gridVrms = ref(230)
const gridHz = ref(50)
const touchLimitMa = ref(3.5)   // compliance tier: bounds C_Y and scores the touch verdict
const design = ref(null)
const netlist = ref('')
const netlistMode = ref('dm')
const error = ref('')
const ilCm = ref(null)
const ilDm = ref(null)
const worstCaseAt = ref(null)   // {cm: {standard, worst}, dm: {...}} at f_design
const escalated = ref(false)    // selector had to go beyond the asymptote sizing
const bindingSets = ref(null)   // receiver handoff: {cm: [[f,A]...], dm: [[f,A]...]}
const bindingNote = ref('')
const fCritCmHz = ref(null)     // per-mode critical design frequency from the binding sets
const fCritDmHz = ref(null)

// ── output panes (Kirchhoff pattern: two, independently switchable) ──────────
const PANE_VIEWS = [
  ['schematic', 'SCHEMATIC'],
  ['parts', 'CATALOG PARTS'],
  ['bom', 'BOM & EXPORT'],
  ['il', 'INSERTION LOSS'],
  ['values', 'SIZING & SAFETY'],
  ['netlist', 'SPICE NETLIST'],
  ['lisn', 'TEST SETUP (LISN)'],
]
const paneA = ref('schematic')
const paneB = ref('il')
const openSection = ref('req')   // KH-style accordion: one input stage open at a time
const netlistLisn = ref('cispr16')   // which artificial network the SPICE deck embeds

// ── templates: real worked examples to start from ────────────────────────────
// The ANP015 preset reproduces the note's numbers exactly (the engine's golden
// test vector); the others are TYPICAL setups from the application literature —
// their attenuation targets are starting points, meant to be replaced by YOUR
// scan via the Spectrum/Receiver hand-off.
const EXAMPLES = {
  anp015: {
    label: 'WE ANP015 worked example — 300 kHz flyback, CISPR 32 B',
    note: 'ANP015 rev. 2024-06 §worked example: 40 dB at 300 kHz, C_Y 4.7 nF, leakage from ' +
      '|Z| = 92 Ω @ 1 MHz — expect 3.3 mH + 2.2 µF, as printed in the note.',
    apply: (r) => { r.fSwKhz = 300; r.aReqCm = 40; r.aReqDm = 40; r.cYnF = '4.7'; r.stages = 1
      r.dmMode = 'impedance'; r.dmImpedanceOhm = 92; r.dmImpedanceMhz = 1; r.gridVrms = 230; r.gridHz = 50 },
  },
  adapter65: {
    label: 'Typical 65 kHz offline flyback (notebook adapter), CISPR 32 B',
    note: 'Typical adapter-class starting point (65 kHz PWM → the tool designs at the first ' +
      'in-band harmonic, 195 kHz). Replace the targets with your own scan via Spectrum/Receiver.',
    apply: (r) => { r.fSwKhz = 65; r.aReqCm = 36; r.aReqDm = 30; r.cYnF = 'auto'; r.stages = 1
      r.dmMode = 'inductance'; r.lDmUh = 20; r.gridVrms = 230; r.gridHz = 50 },
  },
  drive16: {
    label: 'Typical 16 kHz industrial drive input, 2-stage, CISPR 11 A',
    note: 'Typical drive-class starting point (16 kHz PWM → designed at 160 kHz, two stages ' +
      'for the heavy low-frequency comb). Replace the targets with your own scan.',
    apply: (r) => { r.fSwKhz = 16; r.aReqCm = 50; r.aReqDm = 45; r.cYnF = 'auto'; r.stages = 2
      r.dmMode = 'inductance'; r.lDmUh = 15; r.gridVrms = 230; r.gridHz = 50 },
  },
}
const exampleId = ref('')
const exampleNote = ref('')
function applyExample(id) {
  if (!EXAMPLES[id]) return
  clearBinding()
  const refs = {
    set fSwKhz(v) { fSwKhz.value = v }, set aReqCm(v) { aReqCm.value = v },
    set aReqDm(v) { aReqDm.value = v }, set cYnF(v) { cYnF.value = v },
    set stages(v) { stages.value = v }, set dmMode(v) { dmMode.value = v },
    set dmImpedanceOhm(v) { dmImpedanceOhm.value = v }, set dmImpedanceMhz(v) { dmImpedanceMhz.value = v },
    set lDmUh(v) { lDmUh.value = v }, set gridVrms(v) { gridVrms.value = v },
    set gridHz(v) { gridHz.value = v },
  }
  EXAMPLES[id].apply(refs)
  exampleNote.value = EXAMPLES[id].note
  openSection.value = 'req'
  compute()
}
function onPaneChange(which, view) {
  // never show the same view twice — swap instead
  if (which === 'a') {
    if (paneB.value === view) paneB.value = paneA.value
    paneA.value = view
  } else {
    if (paneA.value === view) paneA.value = paneB.value
    paneB.value = view
  }
}
function selectComponent(ref_) {
  selectedRef.value = ref_
  // clicking a schematic component surfaces its catalog in the OTHER pane
  if (paneA.value !== 'parts' && paneB.value !== 'parts') {
    onPaneChange(paneA.value === 'schematic' ? 'b' : 'a', 'parts')
  }
}

// Reduce EACH MODE's binding set to the (f*, A*) whose f/10^(A/(40 n)) is that
// set's minimum — the mode's own cutoff formula then meets every one of ITS
// binding points. CM and DM stay separate all the way: sizing the choke from a
// DM offence (or the X cap from a CM one) over-designs the quiet mode.
function reduceBindingSet(set, n) {
  if (!set?.length) return null
  let best = set[0]
  for (const point of set) {
    if (point[0] / 10 ** (point[1] / (40 * n)) < best[0] / 10 ** (best[1] / (40 * n))) best = point
  }
  return best
}

function deriveFromBindings() {
  const n = Number(stages.value)
  const cm = reduceBindingSet(bindingSets.value.cm, n)
  const dm = reduceBindingSet(bindingSets.value.dm, n)
  if (!cm && !dm) {
    // a hand-off with nothing binding must NOT silently present the form
    // defaults as if they were a derived design
    bindingNote.value = 'the receiver hand-off contains no binding points — every measured '
      + 'frequency clears the limit with the full buffer in hand; no filter is required, '
      + 'and nothing was derived (the form below is a blank designer)'
    fCritCmHz.value = null
    fCritDmHz.value = null
    return false
  }
  // a silent mode needs 0 dB — evaluated at the other mode's critical
  // frequency, which is the same statement at any frequency
  aReqCm.value = cm ? Math.ceil(cm[1]) : 0
  aReqDm.value = dm ? Math.ceil(dm[1]) : 0
  fCritCmHz.value = cm ? cm[0] : dm[0]
  fCritDmHz.value = dm ? dm[0] : cm[0]
  fSwKhz.value = Math.round(Math.min(fCritCmHz.value, fCritDmHz.value)) / 1e3
  const label = (mode, best, points) => best
    ? `${mode} ${Math.ceil(best[1])} dB @ ${(best[0] / 1e3).toFixed(0)} kHz (${points} pts)`
    : `${mode} silent`
  bindingNote.value = 'sized per mode by the min-f_co rule — '
    + label('CM', cm, bindingSets.value.cm?.length ?? 0) + ' · '
    + label('DM', dm, bindingSets.value.dm?.length ?? 0) + ` · ${n}-stage`
  return true
}

// Any manual edit of the requirement fields leaves hand-off mode: the derived
// per-mode design frequencies must never silently ride along under new numbers.
function clearBinding() {
  if (!bindingSets.value) return
  bindingSets.value = null
  bindingNote.value = ''
  fCritCmHz.value = null
  fCritDmHz.value = null
}

watch(stages, () => {
  if (bindingSets.value && deriveFromBindings()) compute()
})
const vInMin = ref(207)
const vInMinDirty = ref(false)
const pIn = ref(25)
const interaction = ref(null)
const lCmSource = ref('manual')      // 'manual' | 'catalog'
const catalog = ref(null)            // {count, parts:[{mpn,manufacturer,family,inductanceH,ratedCurrentA,dcrOhm}]}
const catalogState = ref('loading')  // 'loading' | 'ready' | 'unavailable'
const mfrFilter = ref('')
const minRatedA = ref(1)
const capsCatalog = ref(null)
const selectedRef = ref('')
const bindings = ref({})
const curvesFile = ref(null)     // /kelvin/hertz-cmc-curves.v1.json, fetched on first need
const measured = ref(null)       // {mpn, cm?: {f, db}, dm?: {f, db}} — bound part's measured-Z IL
const measuredIlAt = ref({})     // mpn -> measured CM IL (dB) at f_design, for the parts pane

// ── Y capacitor auto-selection ───────────────────────────────────────────────
// The touch-current budget CAPS C_Y; within that cap, BIGGER C_Y means a
// SMALLER (cheaper) choke for the same attenuation — so 'auto' picks the
// largest standard value whose worst-case leakage (V+10 %, C+20 %, per-line,
// per IEC 60990) stays under the selected tier. The spectrum never picks C_Y:
// it sets the attenuation requirement; safety sets the C_Y ceiling.
const CY_STANDARD_NF = [1, 2.2, 3.3, 4.7, 10, 15, 22, 33, 47]
const autoCyNf = computed(() => {
  const budgetF = (Number(touchLimitMa.value) * 1e-3) /
    (Number(gridVrms.value) * 1.1 * 2 * Math.PI * Number(gridHz.value) * 1.2 * Number(stages.value))
  const fit = CY_STANDARD_NF.filter((nf) => nf * 1e-9 <= budgetF)
  return fit.length ? fit[fit.length - 1] : null
})
function resolvedCyNf() {
  if (cYnF.value !== 'auto') return Number(cYnF.value)
  if (autoCyNf.value === null) {
    throw new Error(`even the smallest standard Y capacitor exceeds the ${touchLimitMa.value} mA ` +
      'touch budget at this grid — pick C_Y manually or relax the tier')
  }
  return autoCyNf.value
}

onMounted(async () => {
  try {
    const response = await fetch('/kelvin/hertz-cmc.v1.json')
    if (!response.ok) throw new Error(String(response.status))
    catalog.value = await response.json()
    catalogState.value = 'ready'
  } catch {
    catalogState.value = 'unavailable'
  }
  try {
    const response = await fetch('/kelvin/hertz-safety-caps.v1.json')
    if (response.ok) capsCatalog.value = await response.json()
  } catch { /* caps panes show their own unavailable state */ }
  if (store.handoff) {
    if (store.handoff.binding) {
      bindingSets.value = store.handoff.binding
      if (!deriveFromBindings()) { store.handoff = null; return }
    } else {
      aReqCm.value = store.handoff.aReqCmDb ?? store.handoff.aReqDb
      aReqDm.value = store.handoff.aReqDmDb ?? store.handoff.aReqDb
      if (store.handoff.fSwHz) fSwKhz.value = Math.round(store.handoff.fSwHz / 1e3)
    }
    store.handoff = null
    compute()
  }
})

const manufacturers = () =>
  catalog.value ? [...new Set(catalog.value.parts.map((p) => p.manufacturer))].sort() : []

function catalogPartsAnyL() {
  if (!catalog.value) return []
  // an UNRATED part can never satisfy a positive current requirement — letting
  // null bypass the threshold put an 11.5 A SMD choke into a "100 A" design.
  // Set the threshold to 0 to browse unrated parts explicitly.
  const minRated = Number(minRatedA.value)
  return catalog.value.parts.filter((p) =>
    (!mfrFilter.value || p.manufacturer === mfrFilter.value) &&
    (p.ratedCurrentA !== null ? p.ratedCurrentA >= minRated : minRated <= 0))
}

function catalogParts() {
  return catalogPartsAnyL().filter((p) => p.inductanceH > 0)
}

// X2 safety capacitors as the C_X candidate source — same pattern as the choke
// catalog: real available values instead of a hand-typed list.
const cxManufacturers = () => [...new Set((capsCatalog.value?.parts ?? [])
  .filter((p) => p.safetyClass === 'X2').map((p) => p.manufacturer))].sort()
function cxCatalogCandidates() {
  const values = [...new Set((capsCatalog.value?.parts ?? [])
    .filter((p) => p.safetyClass === 'X2' && (!cxMfr.value || p.manufacturer === cxMfr.value))
    .map((p) => p.capacitanceF))].sort((a, b) => a - b)
  if (!values.length) throw new Error('no X2 catalog capacitors match the manufacturer filter — loosen it or use the manual list')
  return values
}

async function ensureCurves() {
  if (curvesFile.value) return curvesFile.value
  const response = await fetch('/kelvin/hertz-cmc-curves.v1.json')
  if (!response.ok) throw new Error(String(response.status))
  curvesFile.value = await response.json()
  return curvesFile.value
}

// Measured CM insertion loss at the design frequency for every curve-carrying
// candidate — the SILENT selection column. Value at the NEAREST measured point;
// parts whose curve does not span f_design show none (no extrapolation).
async function loadMeasuredIlColumn() {
  if (!design.value) return
  try {
    const curves = (await ensureCurves()).curves
    const engine = await api()
    const fDesign = design.value.fDesignCmHz
    const column = {}
    for (const part of catalogPartsAnyL()) {
      if (!part.hasMeasuredCmCurve) continue
      const cm = curves[part.mpn]?.cm
      if (!cm || fDesign < cm.f[0] || fDesign > cm.f[cm.f.length - 1]) continue
      const il = engine.measuredIlCurves(cm.f, cm.re, cm.im, design.value.cYgF,
                                         design.value.stages, 25)
      let nearest = 0
      for (let k = 1; k < il.frequenciesHz.length; k += 1) {
        if (Math.abs(il.frequenciesHz[k] - fDesign) <
            Math.abs(il.frequenciesHz[nearest] - fDesign)) nearest = k
      }
      column[part.mpn] = il.standardDb[nearest]
    }
    measuredIlAt.value = column
  } catch { /* column stays empty; parts remain selectable by value */ }
}

function catalogCandidates() {
  const parts = catalogParts()
  if (!parts.length) throw new Error('no catalog parts match the manufacturer/current filter')
  return [...new Set(parts.map((p) => p.inductanceH))].sort((a, b) => a - b)
}

const matchedParts = () => (!design.value || !catalog.value) ? [] : catalogParts()
  .filter((p) => Math.abs(p.inductanceH - design.value.lCmSelectedH) < 1e-3 * design.value.lCmSelectedH)
  .sort((a, b) => (b.ratedCurrentA ?? 0) - (a.ratedCurrentA ?? 0))
  .slice(0, 6)

function parseList(text, scale) {
  const values = text.split(/[,;\s]+/).filter(Boolean).map(Number)
  if (!values.length || values.some((x) => !Number.isFinite(x) || x <= 0)) {
    throw new Error('candidate list must be positive numbers')
  }
  return values.map((x) => x * scale)
}

// Multi-point DM leakage: each (f, |Z|) in the inductive region gives
// L = Z/(2πf); the median is used, and a spread across the points is
// SURFACED — a large spread means the points straddle the self-resonance.
function fitDmPoints() {
  const rows = dmPointsText.value.split('\n').map((line) => line.trim()).filter(Boolean)
  const inductances = rows.map((line) => {
    const [fMhz, zOhm] = line.split(/[,;\s]+/).map(Number)
    if (!Number.isFinite(fMhz) || !Number.isFinite(zOhm) || fMhz <= 0 || zOhm <= 0) {
      throw new Error(`DM point "${line}" is not "MHz, Ohm" with positive numbers`)
    }
    return zOhm / (2 * Math.PI * fMhz * 1e6)
  })
  if (inductances.length < 2) throw new Error('give at least two "MHz, Ohm" DM impedance points')
  const sorted = [...inductances].sort((a, b) => a - b)
  const spread = sorted[sorted.length - 1] / sorted[0]
  if (spread > 2) {
    throw new Error(`the DM points do not lie on one inductive slope (L varies ${spread.toFixed(1)}×) — ` +
      'use points below the choke\'s self-resonance')
  }
  dmPointsNote.value = spread > 1.25
    ? `L spread ${((spread - 1) * 100).toFixed(0)} % across the DM points — median used; ` +
      'the highest-frequency points may already feel the SRF'
    : ''
  return sorted[Math.floor(sorted.length / 2)]
}

async function compute() {
  error.value = ''
  design.value = null
  netlist.value = ''
  escalated.value = false
  interaction.value = null
  ilCm.value = null
  ilDm.value = null
  worstCaseAt.value = null
  dmPointsNote.value = ''
  try {
    const engine = await api()
    if (lCmSource.value === 'catalog' && catalogState.value === 'ready' && !catalogCandidates().length) {
      throw new Error('the manufacturer / rated-current filters removed every catalog part — loosen them')
    }
    const params = {
      fSwHz: fSwKhz.value * 1e3,
      aReqCmDb: Number(aReqCm.value),
      aReqDmDb: Number(aReqDm.value),
      cYPerLineF: resolvedCyNf() * 1e-9,
      stages: Number(stages.value),
      lCmCandidatesH: lCmSource.value === 'catalog' && catalogState.value === 'ready'
        ? catalogCandidates() : parseList(lCandidatesMh.value, 1e-3),
      cXCandidatesF: cxSource.value === 'catalog' && capsCatalog.value
        ? cxCatalogCandidates() : parseList(cxCandidatesUf.value, 1e-6),
      grid: { vRms: Number(gridVrms.value), fHz: Number(gridHz.value), vSafe: 60, tDischargeS: 1 },
    }
    if (bindingSets.value && fCritCmHz.value && fCritDmHz.value) {
      params.fDesignCmHz = fCritCmHz.value
      params.fDesignDmHz = fCritDmHz.value
    }
    if (dmMode.value === 'impedance') {
      params.dmImpedanceOhm = Number(dmImpedanceOhm.value)
      params.dmImpedanceFrequencyHz = dmImpedanceMhz.value * 1e6
    } else if (dmMode.value === 'points') {
      params.lDmH = fitDmPoints()
    } else {
      params.lDmH = lDmUh.value * 1e-6
    }
    bindings.value = {}
    selectedRef.value = ''
    measured.value = null
    measuredIlAt.value = {}
    // The verdict criterion is the nominal in-circuit insertion loss (25 Ω CM /
    // 100 Ω DM — the terminations a CISPR 16 AMN actually presents). The ANP015
    // asymptote only SIZES; if the sized part misses the in-circuit criterion,
    // escalate through the candidate list until it passes or runs out.
    const evaluate = (p) => {
      const d = engine.designFilter(p)
      // the chip must be evaluated AT f_design — never snapped to a span edge
      const fLow = Math.min(d.fDesignCmHz, d.fDesignDmHz)
      const fHigh = Math.max(d.fDesignCmHz, d.fDesignDmHz)
      const span = { fMinHz: Math.min(150e3, fLow / 2),
                     fMaxHz: Math.max(30e6, fHigh * 2), pointsPerDecade: 30 }
      const cm = engine.insertionLossCurves({ inductanceH: d.lCmSelectedH, capacitanceF: d.cYgF,
        stages: d.stages, referenceImpedanceOhm: 25, ...span })
      const dm = engine.insertionLossCurves({ inductanceH: d.lDmH, capacitanceF: d.cXSelectedF,
        stages: d.stages, referenceImpedanceOhm: 100, ...span })
      // the chip is a hard pass/fail input: evaluate it AT f_design via a
      // micro-span, not at the nearest 30-per-decade grid point (<=1.35 dB off)
      const exactAt = (inductanceH, capacitanceF, refZ, fDesignHz) => {
        const il = engine.insertionLossCurves({ inductanceH, capacitanceF, stages: d.stages,
          referenceImpedanceOhm: refZ, fMinHz: fDesignHz * 0.9995, fMaxHz: fDesignHz * 1.0005,
          pointsPerDecade: 20000 })
        const mid = Math.floor(il.frequenciesHz.length / 2)
        return { standard: il.standardDb[mid], worst: il.worstCaseDb[mid] }
      }
      return { d, cm, dm, at: { cm: exactAt(d.lCmSelectedH, d.cYgF, 25, d.fDesignCmHz),
                                dm: exactAt(d.lDmH, d.cXSelectedF, 100, d.fDesignDmHz) } }
    }
    // in-circuit IL at exactly f_design — the same criterion the verdict chip
    // scores, so the escalation target and the verdict can never disagree
    const ilAtDesign = (inductanceH, capacitanceF, refZ, fDesignHz, nStages) => {
      const il = engine.insertionLossCurves({ inductanceH, capacitanceF, stages: nStages,
        referenceImpedanceOhm: refZ, fMinHz: fDesignHz * 0.9995, fMaxHz: fDesignHz * 1.0005,
        pointsPerDecade: 20000 })
      return il.standardDb[Math.floor(il.frequenciesHz.length / 2)]
    }
    let attempt = evaluate(params)
    for (let guard = 0; guard < 4; guard += 1) {
      const needCm = attempt.at.cm.standard < Number(params.aReqCmDb)
      const needDm = attempt.at.dm.standard < Number(params.aReqDmDb)
      if (!needCm && !needDm) break
      const next = { ...params }
      let changed = false
      // Jump straight to the SMALLEST candidate meeting the in-circuit
      // criterion (or to the largest available, reported honestly as a
      // shortfall) — stepping one value per iteration never crossed a
      // 600-value parts catalog within any sane iteration cap.
      if (needCm) {
        const larger = params.lCmCandidatesH.filter((v) => v > attempt.d.lCmSelectedH).sort((a, b) => a - b)
        if (larger.length) {
          const pass = larger.find((v) =>
            ilAtDesign(v, attempt.d.cYgF, 25, attempt.d.fDesignCmHz, attempt.d.stages) >= Number(params.aReqCmDb))
          next.lCmCandidatesH = pass !== undefined
            ? params.lCmCandidatesH.filter((v) => v >= pass) : [larger[larger.length - 1]]
          changed = true
        }
      }
      if (needDm) {
        const larger = params.cXCandidatesF.filter((v) => v > attempt.d.cXSelectedF).sort((a, b) => a - b)
        if (larger.length) {
          const pass = larger.find((v) =>
            ilAtDesign(attempt.d.lDmH, v, 100, attempt.d.fDesignDmHz, attempt.d.stages) >= Number(params.aReqDmDb))
          next.cXCandidatesF = pass !== undefined
            ? params.cXCandidatesF.filter((v) => v >= pass) : [larger[larger.length - 1]]
          changed = true
        }
      }
      if (!changed) break
      Object.assign(params, next)
      attempt = evaluate(params)
      escalated.value = true
    }
    design.value = attempt.d
    ilCm.value = attempt.cm
    ilDm.value = attempt.dm
    worstCaseAt.value = attempt.at
    const d = design.value
    interaction.value = engine.inputFilterInteraction(d.lDmH, d.cXSelectedF,
      Number(vInMin.value), Number(pIn.value))
    try {
      netlist.value = engine.filterSpiceNetlist(design.value, netlistLisn.value, netlistMode.value)
    } catch (netlistError) {
      netlist.value = '* netlist unavailable: ' + netlistError.message
    }
  } catch (e) {
    error.value = e.message
  }
}

watch(gridVrms, (v) => {
  if (!vInMinDirty.value && v > 0) vInMin.value = Math.round(0.9 * v)
})

const ilSeries = () => [
  ...(measured.value?.cm ? [{ id: 'cmm', label: `CM measured (${measured.value.mpn})`, color: 'var(--s-1)', dash: '1 4',
    points: measured.value.cm.f.map((f, i) => ({ f, v: measured.value.cm.db[i] })) }] : []),
  ...(measured.value?.dm ? [{ id: 'dmm', label: `DM measured (${measured.value.mpn})`, color: 'var(--s-2)', dash: '1 4',
    points: measured.value.dm.f.map((f, i) => ({ f, v: measured.value.dm.db[i] })) }] : []),
  { id: 'cm', label: 'CM in-circuit (25 Ω)', color: 'var(--s-1)',
    points: ilCm.value.frequenciesHz.map((f, i) => ({ f, v: ilCm.value.standardDb[i] })) },
  { id: 'cmw', label: 'CM worst case (CISPR 17)', color: 'var(--s-1)', dash: '3 4',
    points: ilCm.value.frequenciesHz.map((f, i) => ({ f, v: ilCm.value.worstCaseDb[i] })) },
  { id: 'dm', label: 'DM in-circuit (100 Ω)', color: 'var(--s-2)',
    points: ilDm.value.frequenciesHz.map((f, i) => ({ f, v: ilDm.value.standardDb[i] })) },
  { id: 'dmw', label: 'DM worst case (CISPR 17)', color: 'var(--s-2)', dash: '3 4',
    points: ilDm.value.frequenciesHz.map((f, i) => ({ f, v: ilDm.value.worstCaseDb[i] })) },
]
const requirementMarkers = () => [
  { f: design.value.fDesignCmHz, v: Number(aReqCm.value), color: 'var(--s-1)' },
  { f: design.value.fDesignDmHz, v: Number(aReqDm.value), color: 'var(--s-2)' },
]

const schematicLabels = () => {
  const labels = {}
  for (let s = 1; s <= design.value.stages; s += 1) {
    labels[`CMC${s}`] = fmtSi(design.value.lCmSelectedH, 'H')
    labels[`C_X${s}`] = fmtSi(design.value.cXSelectedF, 'F')
    labels[`C_YL${s}`] = fmtSi(design.value.cYPerLineF, 'F')
    labels[`C_YN${s}`] = fmtSi(design.value.cYPerLineF, 'F')
  }
  return labels
}

const kindOf = (ref) => ref.startsWith('CMC') ? 'cmc' : ref.startsWith('C_X') ? 'cx' : 'cy'
const targetValueOf = (kind) => kind === 'cmc' ? design.value.lCmSelectedH
  : kind === 'cx' ? design.value.cXSelectedF : design.value.cYPerLineF

// Catalog recommendations for the selected component: closest values first,
// exact matches naturally leading. Identical stages share bindings, so binding
// one CMC binds them all (and Y caps bind as the full set).
const recommendations = () => {
  if (!selectedRef.value || !design.value) return null
  const kind = kindOf(selectedRef.value)
  const target = targetValueOf(kind)
  let pool
  if (kind === 'cmc') {
    // honor the catalog-mode manufacturer/current filters the user set; parts
    // without an inductance ride along when they carry a measured curve
    const base = lCmSource.value === 'catalog' ? catalogPartsAnyL()
      : (catalog.value?.parts ?? [])
    const valued = base.filter((p) => p.inductanceH > 0)
      .map((p) => ({ ...p, valueF: p.inductanceH, deviation: Math.abs(p.inductanceH - targetValueOf('cmc')) / targetValueOf('cmc') }))
      .sort((a, b) => (a.ratedCurrentA === null) - (b.ratedCurrentA === null) ||
        a.deviation - b.deviation || (b.ratedCurrentA ?? 0) - (a.ratedCurrentA ?? 0))
      .slice(0, 6)
    const impedanceOnly = base.filter((p) => !(p.inductanceH > 0) && measuredIlAt.value[p.mpn] !== undefined)
      .map((p) => ({ ...p, valueF: null, deviation: null }))
      .sort((a, b) => measuredIlAt.value[b.mpn] - measuredIlAt.value[a.mpn])
      .slice(0, 4)
    const parts = [...valued, ...impedanceOnly]
    if (!parts.length) return { kind, target: targetValueOf('cmc'), parts: [], unavailable: !base.length }
    return { kind, target: targetValueOf('cmc'), parts, unavailable: false }
  }
  const cls = kind === 'cx' ? 'X2' : 'Y2'
  pool = (capsCatalog.value?.parts ?? [])
    .filter((p) => p.safetyClass === cls)
    .map((p) => ({ ...p, valueF: p.capacitanceF }))
  if (!pool.length) return { kind, target, parts: [], unavailable: true }
  const parts = pool
    .map((p) => ({ ...p, deviation: Math.abs(p.valueF - target) / target }))
    .sort((a, b) => a.deviation - b.deviation || String(a.mpn).localeCompare(String(b.mpn)))
    .slice(0, 8)
  return { kind, target, parts, unavailable: false }
}

watch(selectedRef, (ref_) => {
  if (ref_ && kindOf(ref_) === 'cmc') loadMeasuredIlColumn()
})

async function bindPart(part) {
  const kind = kindOf(selectedRef.value)
  for (const c of filterComponents(design.value.stages)) {
    if (c.kind === kind) bindings.value[c.ref] = { mpn: part.mpn, manufacturer: part.manufacturer,
      maxRatedV: Math.max(part.ratedVoltageAcV ?? 0, part.ratedVoltageDcV ?? 0) || null }
  }
  bindings.value = { ...bindings.value }
  if (kind !== 'cmc') return
  measured.value = null
  if (!part.hasMeasuredCmCurve && !part.hasMeasuredDmCurve) return
  try {
    const curve = (await ensureCurves()).curves[part.mpn]
    if (!curve) return
    const engine = await api()
    // trim measured points to the chart span — subsetting, never extending
    const trim = (mode) => {
      const keep = mode.f.map((f, i) => [f, i]).filter(([f]) => f >= 150e3 && f <= 30e6)
      return { f: keep.map(([f]) => f), re: keep.map(([, i]) => mode.re[i]), im: keep.map(([, i]) => mode.im[i]) }
    }
    const result = { mpn: part.mpn }
    if (curve.cm) {
      const t = trim(curve.cm)
      if (t.f.length >= 2) {
        const il = engine.measuredIlCurves(t.f, t.re, t.im, design.value.cYgF, design.value.stages, 25)
        result.cm = { f: il.frequenciesHz, db: il.standardDb }
      }
    }
    if (curve.dm) {
      const t = trim(curve.dm)
      if (t.f.length >= 2) {
        const il = engine.measuredIlCurves(t.f, t.re, t.im, design.value.cXSelectedF, design.value.stages, 100)
        result.dm = { f: il.frequenciesHz, db: il.standardDb }
      }
    }
    if (result.cm || result.dm) measured.value = result
  } catch (e) {
    error.value = 'measured-curve overlay unavailable: ' + e.message
  }
}

// CMCs only, by design: the caps catalog's ratedVoltageV mixes AC-class and DC
// ratings, and comparing an X2's 305 VAC class number to the grid PEAK would
// false-alarm on nearly every legitimate safety cap. Do not "fix" this.
const bindingVoltageWarning = (binding) => {
  if (!binding || !binding.maxRatedV) return null
  const peak = Number(gridVrms.value) * Math.SQRT2
  return binding.maxRatedV < peak
    ? `rated ${binding.maxRatedV} V < ${Math.round(peak)} V grid peak — NOT a mains part` : null
}
const bomRows = () => filterComponents(design.value.stages).map((c) => ({
  ref: c.ref,
  value: schematicLabels()[c.ref],
  binding: bindings.value[c.ref] ?? null,
  warning: bindingVoltageWarning(bindings.value[c.ref]),
}))
const allBound = () => filterComponents(design.value.stages).every((c) => bindings.value[c.ref]?.mpn)

// A coupled pair cannot leak more than its full loop: K = 1 - L_dm/(2 L_cm)
// must stay in (0,1). The netlist already refuses; the design panel must not
// present an unbuildable BOM as valid either.
const unrealizable = () => design.value && design.value.lDmH >= 2 * design.value.lCmSelectedH

function downloadCias() {
  const brick = buildFilterCias(design.value.stages, bindings.value)
  const blob = new Blob([JSON.stringify(brick, null, 2)], { type: 'application/json' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = brick.name + '.cias.json'
  a.click()
  URL.revokeObjectURL(a.href)
}

function copyNetlist() {
  navigator.clipboard.writeText(netlist.value)
}

function downloadNetlist() {
  const blob = new Blob([netlist.value], { type: 'text/plain' })
  const a = document.createElement('a')
  a.href = URL.createObjectURL(blob)
  a.download = 'hertz_line_filter.cir'
  a.click()
  URL.revokeObjectURL(a.href)
}
</script>

<template>
  <div class="fbench">
    <!-- ── input rail ─────────────────────────────────────────────────────── -->
    <aside class="fcontrols panel">
      <div class="rail-head">
        <button class="act" data-test="compute" @click="compute">DESIGN FILTER</button>
        <select class="example-select" data-test="example-select" :value="exampleId"
                @change="applyExample($event.target.value); $event.target.value = ''">
          <option value="" disabled selected>load a template…</option>
          <option v-for="(ex, id) in EXAMPLES" :key="id" :value="id">{{ ex.label }}</option>
        </select>
      </div>
      <p v-if="exampleNote" class="note" data-test="example-note">{{ exampleNote }}</p>

      <button class="acc-head" :class="{ open: openSection === 'req' }" data-test="sec-req"
              @click="openSection = 'req'">1 · REQUIREMENT</button>
      <div v-show="openSection === 'req'" class="acc-body">
      <div class="row">
        <label class="field"><span>Switching frequency (kHz)</span>
          <input v-model.number="fSwKhz" type="number" min="1" data-test="fsw" @input="clearBinding" /></label>
        <label class="field"><span>Stages</span>
          <select v-model.number="stages"><option :value="1">1 (40 dB/dec)</option><option :value="2">2 (80 dB/dec)</option></select></label>
      </div>
      <div class="row">
        <label class="field"><span>CM attenuation (dB)</span>
          <input v-model.number="aReqCm" type="number" data-test="areq-cm" @input="clearBinding" /></label>
        <label class="field"><span>DM attenuation (dB)</span>
          <input v-model.number="aReqDm" type="number" data-test="areq-dm" @input="clearBinding" /></label>
      </div>
      <p class="note">From a failed scan: <em>Design the fix</em> (Spectrum) or <em>Design filter for
        these modes</em> (Receiver) fill this section. Below 150 kHz the design moves to the first
        harmonic inside the measured band (ANP015).</p>
      <p v-if="bindingNote" class="note" data-test="binding-note">{{ bindingNote }}</p>
      </div>

      <button class="acc-head" :class="{ open: openSection === 'comp' }" data-test="sec-comp"
              @click="openSection = 'comp'">2 · COMPONENTS</button>
      <div v-show="openSection === 'comp'" class="acc-body">
      <label class="field"><span>Y capacitor per line</span>
        <select v-model="cYnF" data-test="cy-select">
          <option value="auto">auto — largest within the touch budget{{ autoCyNf !== null ? ` (→ ${autoCyNf} nF)` : '' }}</option>
          <option v-for="v in [1, 2.2, 3.3, 4.7, 10]" :key="v" :value="v">{{ v }} nF (manual)</option>
        </select></label>
      <p class="note">Safety caps C_Y (touch current), never the spectrum; within that cap a larger
        C_Y means a smaller choke, so <em>auto</em> takes the largest standard value passing the
        tier under Grid &amp; safety.</p>
      <label class="field"><span>DM (leakage) inductance from</span>
        <select v-model="dmMode" data-test="dm-mode">
          <option value="impedance">one point of the choke's DM |Z| curve</option>
          <option value="points">several DM |Z| points (fitted)</option>
          <option value="inductance">a known leakage inductance</option>
        </select></label>
      <div v-if="dmMode === 'impedance'" class="row">
        <label class="field"><span>|Z| (Ω)</span><input v-model.number="dmImpedanceOhm" type="number" /></label>
        <label class="field"><span>at (MHz)</span><input v-model.number="dmImpedanceMhz" type="number" /></label>
      </div>
      <label v-else-if="dmMode === 'points'" class="field"><span>One “MHz, Ω” per line (inductive region)</span>
        <textarea v-model="dmPointsText" rows="3" data-test="dm-points" placeholder="0.1, 9.2&#10;0.3, 27.5&#10;1, 92"></textarea></label>
      <label v-else class="field"><span>Total loop leakage (µH) — ≈ 2× per-winding</span>
        <input v-model.number="lDmUh" type="number" data-test="ldm-input" /></label>
      <p v-if="dmPointsNote" class="note" style="color: var(--amber)">{{ dmPointsNote }}</p>
      <label class="field"><span>CM choke candidates</span>
        <select v-model="lCmSource" data-test="lcm-source">
          <option value="manual">manual value list</option>
          <option value="catalog" :disabled="catalogState !== 'ready'">
            parts catalog{{ catalogState === 'ready' ? ` (${catalog.count} chokes)` : catalogState === 'loading' ? ' (loading…)' : ' (unavailable here)' }}
          </option>
        </select></label>
      <label v-if="lCmSource === 'manual'" class="field"><span>Values (mH)</span>
        <input v-model="lCandidatesMh" type="text" /></label>
      <div v-else class="row">
        <label class="field"><span>Manufacturer</span>
          <select v-model="mfrFilter" data-test="mfr-filter"><option value="">all manufacturers</option>
            <option v-for="m in manufacturers()" :key="m" :value="m">{{ m }}</option></select></label>
        <label class="field"><span>Min. rated A (0 = incl. unrated)</span>
          <input v-model.number="minRatedA" type="number" min="0" data-test="min-rated" /></label>
      </div>
      <label class="field"><span>X capacitor candidates</span>
        <select v-model="cxSource" data-test="cx-source">
          <option value="manual">manual value list</option>
          <option value="catalog" :disabled="!capsCatalog">
            X2 safety-caps catalog{{ capsCatalog ? '' : ' (unavailable here)' }}
          </option>
        </select></label>
      <label v-if="cxSource === 'manual'" class="field"><span>Values (µF)</span>
        <input v-model="cxCandidatesUf" type="text" /></label>
      <label v-else class="field"><span>Manufacturer</span>
        <select v-model="cxMfr" data-test="cx-mfr"><option value="">all manufacturers</option>
          <option v-for="m in cxManufacturers()" :key="m" :value="m">{{ m }}</option></select></label>
      </div>

      <button class="acc-head" :class="{ open: openSection === 'grid' }" data-test="sec-grid"
              @click="openSection = 'grid'">3 · GRID &amp; SAFETY</button>
      <div v-show="openSection === 'grid'" class="acc-body">
      <div class="row">
        <label class="field"><span>Grid (V RMS)</span><input v-model.number="gridVrms" type="number" /></label>
        <label class="field"><span>Grid (Hz)</span><input v-model.number="gridHz" type="number" /></label>
      </div>
      <label class="field"><span>Touch-current tier (bounds C_Y)</span>
        <select v-model.number="touchLimitMa" data-test="touch-tier">
          <option :value="3.5">3.5 mA — IEC 62368-1 Class I</option>
          <option :value="0.75">0.75 mA — appliance</option>
          <option :value="0.5">0.5 mA — medical</option>
        </select></label>
      <div class="row">
        <label class="field"><span>Converter V<sub>in</sub> min (V)</span>
          <input v-model.number="vInMin" type="number" @input="vInMinDirty = true" @change="compute" /></label>
        <label class="field"><span>Input power (W)</span>
          <input v-model.number="pIn" type="number" @change="compute" /></label>
      </div>
      <p class="note">V<sub>in</sub>/P feed the Middlebrook stability check; the tier bounds C_Y
        and scores the touch-current verdict.</p>
      </div>

      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </aside>

    <!-- ── workspace: verdict strip + two switchable panes ────────────────── -->
    <main class="fworkspace">
      <div v-if="design" class="fstrip panel-hi">
        <div class="fstrip-row">
          <span v-if="worstCaseAt" class="chip" :class="worstCaseAt.cm.standard >= Number(aReqCm) ? 'pass' : 'fail'" data-test="wc-verdict-cm">
            CM {{ fmtDb(worstCaseAt.cm.standard) }} dB {{ worstCaseAt.cm.standard >= Number(aReqCm) ? '≥' : '<' }} {{ fmtDb(Number(aReqCm), 0) }}</span>
          <span v-if="worstCaseAt" class="chip" :class="worstCaseAt.dm.standard >= Number(aReqDm) ? 'pass' : 'fail'" data-test="wc-verdict-dm">
            DM {{ fmtDb(worstCaseAt.dm.standard) }} dB {{ worstCaseAt.dm.standard >= Number(aReqDm) ? '≥' : '<' }} {{ fmtDb(Number(aReqDm), 0) }}</span>
          <span v-if="interaction" class="chip" :class="interaction.marginDb >= 12 ? 'pass' : interaction.marginDb >= 6 ? 'warn' : 'fail'"
                data-test="middlebrook-margin">STABILITY {{ fmtDb(interaction.marginDb) }} dB</span>
          <span v-if="design.leakageCurrentA !== undefined" class="chip"
                :class="design.leakageCurrentA < touchLimitMa * 1e-3 ? 'pass' : 'fail'" data-test="touch-verdict">
            TOUCH {{ fmtSi(design.leakageCurrentA, 'A') }} {{ design.leakageCurrentA < touchLimitMa * 1e-3 ? '<' : '≥' }} {{ touchLimitMa }} mA</span>
        </div>
        <div class="fstrip-row cells">
          <span class="fcell"><b>f<sub>design</sub></b>
            <span v-if="design.fDesignCmHz === design.fDesignDmHz" data-test="f-design">{{ fmtHz(design.fDesignCmHz) }}</span>
            <span v-else data-test="f-design">CM {{ fmtHz(design.fDesignCmHz) }} · DM {{ fmtHz(design.fDesignDmHz) }}</span></span>
          <span class="fcell"><b>f<sub>co</sub></b>
            <span v-if="design.fCutoffCmHz === design.fCutoffDmHz">{{ fmtHz(design.fCutoffCmHz) }}</span>
            <span v-else>CM {{ fmtHz(design.fCutoffCmHz) }} · DM {{ fmtHz(design.fCutoffDmHz) }}</span></span>
          <span class="fcell"><b>L<sub>CM</sub></b><span data-test="lcm">{{ fmtSi(design.lCmSelectedH, 'H') }}</span></span>
          <span class="fcell"><b>C<sub>X</sub></b><span>{{ fmtSi(design.cXSelectedF, 'F') }}</span></span>
          <span class="fcell"><b>C<sub>Y</sub></b><span>2×{{ fmtSi(design.cYPerLineF, 'F') }}/stage</span></span>
          <span v-if="design.dischargeResistorOhm" class="fcell"><b>R<sub>bleed</sub></b>
            <span>{{ fmtSi(design.dischargeResistorOhm, 'Ω') }} · {{ fmtSi(design.dischargeResistorPowerW, 'W') }}</span></span>
        </div>
        <p v-if="unrealizable()" class="err" data-test="k-warning" style="margin: 0.35rem 0 0">
          Not realizable as drawn: the DM (leakage) inductance {{ fmtSi(design.lDmH, 'H') }} is
          ≥ 2 × the selected CM choke {{ fmtSi(design.lCmSelectedH, 'H') }} — a coupled pair cannot
          leak more than 2·L<sub>CM</sub> (coupling K would leave (0,1)). Enter a real leakage value
          or pick a larger choke; the CIAS export is disabled until this is resolved.</p>
        <p v-if="escalated" class="note" data-test="escalated-note" style="margin: 0.25rem 0 0">
          Asymptote-sized parts missed the in-circuit criterion — the selector escalated to larger
          candidates; the chips score what was actually selected.</p>
      </div>

      <div class="fpane-grid">
        <section v-for="(pane, idx) in [paneA, paneB]" :key="idx" class="fpane panel">
          <div class="pane-head">
            <select class="pane-select" :data-test="'pane-select-' + (idx ? 'b' : 'a')" :value="pane"
                    @change="onPaneChange(idx ? 'b' : 'a', $event.target.value)">
              <option v-for="[id, label] in PANE_VIEWS" :key="id" :value="id">{{ label }}</option>
            </select>
          </div>
          <div class="pane-body">
            <!-- schematic -->
            <template v-if="pane === 'schematic'">
              <div v-if="!design" class="pane-empty">
                <div class="ws-title">LINE FILTER</div>
                <div class="ws-sub">ANP015 sizing · in-circuit verdicts · real catalog parts</div>
                <div class="ws-hint">Set the requirement (or bring one over from a failed scan on
                  the Spectrum or Receiver screens) and press <b>DESIGN FILTER</b> — schematic,
                  parts, BOM, insertion loss, safety numbers and SPICE netlist appear in these two
                  switchable panes. The test setup (LISN) view works without a design.</div>
              </div>
              <div v-else class="view-fill">
                <FilterSchematic :stages="design.stages" :labels="schematicLabels()" :bindings="bindings"
                                 :selected="selectedRef" @select="selectComponent" />
                <p class="note" style="flex: 0 0 auto; margin: 0.2rem 0 0">Click a component — its catalog
                  parts open in the other pane. Amber part numbers are bound; identical stages share bindings.</p>
              </div>
            </template>

            <!-- catalog parts for the selected component -->
            <template v-else-if="pane === 'parts'">
              <div v-if="!design || !selectedRef || !recommendations()" class="pane-empty note">
                {{ design ? 'Click a component in the schematic to list catalog parts for it.'
                          : 'Design a filter first — then click a schematic component.' }}</div>
              <div v-else data-test="part-panel">
                <p class="section-label">Parts for {{ selectedRef.replace('C_YL', 'C_Y (pair) ').replace('C_YN', 'C_Y (pair) ') }}
                  — target {{ fmtSi(recommendations().target, kindOf(selectedRef) === 'cmc' ? 'H' : 'F') }}</p>
                <p v-if="recommendations().unavailable" class="note">Parts catalog not reachable from this
                  deployment — bind manually via the BOM later, or design on values only.</p>
                <table v-else class="data">
                  <thead><tr><th>Part</th><th>Manufacturer</th><th>Value</th>
                    <th v-if="kindOf(selectedRef) === 'cmc'">Meas. IL @ f<sub>design</sub></th>
                    <th v-if="kindOf(selectedRef) === 'cmc'">Rated V</th>
                    <th>{{ kindOf(selectedRef) === 'cmc' ? 'Rated A' : 'Class / V' }}</th><th></th></tr></thead>
                  <tbody>
                    <tr v-for="p in recommendations().parts" :key="p.mpn">
                      <td><strong>{{ p.mpn }}</strong></td><td>{{ p.manufacturer }}</td>
                      <td><template v-if="p.valueF !== null">{{ fmtSi(p.valueF, kindOf(selectedRef) === 'cmc' ? 'H' : 'F') }}
                        <span v-if="p.deviation > 0.001" class="note">({{ (p.deviation * 100).toFixed(0) }}% off)</span></template>
                        <span v-else class="note">by measured curve</span></td>
                      <td v-if="kindOf(selectedRef) === 'cmc'" data-test="measured-il"
                          :class="measuredIlAt[p.mpn] !== undefined ? (measuredIlAt[p.mpn] >= Number(aReqCm) ? 'pos' : 'neg') : ''">
                        {{ measuredIlAt[p.mpn] !== undefined ? measuredIlAt[p.mpn].toFixed(1) + ' dB' : '—' }}</td>
                      <td v-if="kindOf(selectedRef) === 'cmc'" class="note">
                        {{ p.ratedVoltageAcV ? p.ratedVoltageAcV + ' VAC' : p.ratedVoltageDcV ? p.ratedVoltageDcV + ' VDC' : 'unrated — verify' }}</td>
                      <td>{{ kindOf(selectedRef) === 'cmc'
                        ? (p.ratedCurrentA !== null && p.ratedCurrentA !== undefined ? p.ratedCurrentA.toFixed(1) : 'unrated — verify')
                        : p.safetyClass + (p.ratedVoltageV ? ' / ' + p.ratedVoltageV + ' V*' : '') }}</td>
                      <td><button class="ghost" data-test="bind-part" @click="bindPart(p)">Use</button></td>
                    </tr>
                  </tbody>
                </table>
                <p v-if="kindOf(selectedRef) !== 'cmc'" class="note">*Datasheet rated voltage — MIXED
                  AC and DC bases: the X2/Y2 class itself is defined for ≤310 VAC mains; values like
                  630 V are DC ratings on the same film part. Verify the AC rating on the datasheet.</p>
                <p v-if="kindOf(selectedRef) === 'cmc'" class="note">
                  Most catalogued chokes carry no voltage rating — for mains use, verify insulation class
                  against the datasheet; chip-scale data-line chokes are never mains parts.
                  “Meas. IL” is computed from the part's measured complex impedance against your Y network
                  at the design frequency (SILENT-style selection); “by measured curve” parts have no
                  catalogued inductance — binding one keeps the designed values in the netlist.</p>
              </div>
            </template>

            <!-- BOM + CIAS export -->
            <template v-else-if="pane === 'bom'">
              <div v-if="!design" class="pane-empty note">Design a filter first.</div>
              <template v-else>
              <p class="section-label">Bill of materials</p>
              <table class="data" data-test="bom">
                <thead><tr><th>Ref</th><th>Value</th><th>Bound part</th></tr></thead>
                <tbody>
                  <tr v-for="row in bomRows()" :key="row.ref">
                    <td>{{ row.ref }}</td><td>{{ row.value }}</td>
                    <td v-if="row.binding"><strong>{{ row.binding.mpn }}</strong> <span class="note">{{ row.binding.manufacturer }}</span>
                      <span v-if="row.warning" style="color: var(--fault)" data-test="bom-voltage-warning"> — {{ row.warning }}</span></td>
                    <td v-else class="note">unbound — click the part in the schematic</td>
                  </tr>
                </tbody>
              </table>
              <div class="row" style="margin-top: 0.6rem">
                <button class="ghost" data-test="download-cias" :disabled="!allBound() || unrealizable()" @click="downloadCias()"
                        :title="unrealizable() ? 'The leakage/choke pair is not physically realizable — fix it first'
                          : allBound() ? 'Download the CIAS circuit brick' : 'Bind every component first — a CIAS with placeholder parts is never produced'">
                  Download CIAS brick
                </button>
              </div>
              <div v-if="lCmSource === 'catalog' && matchedParts().length" style="margin-top: 0.8rem">
                <p class="section-label">Catalog parts at {{ fmtSi(design.lCmSelectedH, 'H') }}</p>
                <table class="data" data-test="catalog-parts">
                  <thead><tr><th>Part</th><th>Manufacturer</th><th>Family</th><th>Rated A</th><th>DCR</th></tr></thead>
                  <tbody>
                    <tr v-for="p in matchedParts()" :key="p.mpn">
                      <td><strong>{{ p.mpn }}</strong></td><td>{{ p.manufacturer }}</td><td>{{ p.family || '—' }}</td>
                      <td>{{ p.ratedCurrentA !== null ? p.ratedCurrentA.toFixed(1) : '—' }}</td>
                      <td>{{ p.dcrOhm !== null ? fmtSi(p.dcrOhm, 'Ω') : '—' }}</td>
                    </tr>
                  </tbody>
                </table>
              </div>
              </template>
            </template>

            <!-- insertion loss -->
            <template v-else-if="pane === 'il'">
              <div v-if="!design" class="pane-empty note">Design a filter first.</div>
              <div v-else-if="ilCm && ilDm">
                <p class="section-label">In-circuit insertion loss — solid: nominal · dashed: CISPR 17 worst case · dot: requirement</p>
                <LogChart :series="ilSeries()" :violations="requirementMarkers()" violation-label="your requirements (CM green, DM blue)" y-label="dB" :height="260" data-test="il-chart" />
                <p class="note">If the dashed worst-case curve still clears your requirement at the design
                  frequency, termination uncertainty cannot eat the margin. Ideal-element curves ignore
                  self-resonance and winding capacitance — above a few MHz a real single-stage filter
                  plateaus at 50–70 dB; bind a part with a measured curve (dotted) for the honest picture.</p>
                <p v-if="measured" class="note" data-test="measured-note">
                  Dotted: predicted with the <strong>measured impedance curve</strong> of {{ measured.mpn }}
                  (complex Z, manufacturer data via the TAS catalog) — shown only over the measured span.</p>
              </div>
            </template>

            <!-- sizing + safety + stability tables -->
            <template v-else-if="pane === 'values'">
              <div v-if="!design" class="pane-empty note">Design a filter first.</div>
              <template v-else>
              <p class="section-label">Common mode — choke against 2×C<sub>Y</sub></p>
              <table class="data">
                <tbody>
                  <tr><td>L<sub>CM</sub> required / selected</td>
                    <td>{{ fmtSi(design.lCmRequiredH, 'H') }} → <strong>{{ fmtSi(design.lCmSelectedH, 'H') }}</strong> per stage</td></tr>
                  <tr><td>C<sub>Y</sub></td><td>2 × {{ fmtSi(design.cYPerLineF, 'F') }} per stage</td></tr>
                  <tr><td>Sizing asymptote (ideal 40·n·log₁₀ — the strip chip is the verdict)</td>
                    <td data-test="il-cm">{{ fmtDb(design.attenuationCmDb) }} dB</td></tr>
                </tbody>
              </table>
              <p class="section-label" style="margin-top: 0.7rem">Differential mode — leakage against C<sub>X</sub></p>
              <table class="data">
                <tbody>
                  <tr><td>L<sub>DM</sub> (leakage)</td><td>{{ fmtSi(design.lDmH, 'H') }}</td></tr>
                  <tr><td>C<sub>X</sub> required / selected</td>
                    <td>{{ fmtSi(design.cXRequiredF, 'F') }} → <strong>{{ fmtSi(design.cXSelectedF, 'F') }}</strong> per stage</td></tr>
                  <tr><td>Sizing asymptote (ideal)</td><td>{{ fmtDb(design.attenuationDmDb) }} dB</td></tr>
                </tbody>
              </table>
              <p class="section-label" style="margin-top: 0.7rem">Safety (worst case: V+10 %, C+20 %)</p>
              <table class="data" v-if="design.leakageCurrentA !== undefined">
                <tbody>
                  <tr><td>PE touch current (single line-side Y path, IEC 60990)</td>
                    <td :class="design.leakageCurrentA < touchLimitMa * 1e-3 ? 'pos' : 'neg'">
                      {{ fmtSi(design.leakageCurrentA, 'A') }} vs {{ touchLimitMa }} mA tier</td></tr>
                  <tr><td>X discharge resistor (60 V in 1 s)</td>
                    <td>≤ {{ fmtSi(design.dischargeResistorMaxOhm, 'Ω') }} → <strong>{{ fmtSi(design.dischargeResistorOhm, 'Ω') }}</strong>
                      ({{ fmtSi(design.dischargeResistorPowerW, 'W') }} continuous — mind standby budgets; use a series
                      pair or an HV-rated part: single chip resistors are typically rated 150–200 V)</td></tr>
                </tbody>
              </table>
              <p class="section-label" style="margin-top: 0.7rem">Input-filter interaction (Middlebrook)</p>
              <table class="data" v-if="interaction">
                <tbody>
                  <tr><td>Filter resonance / R₀ (peak Z<sub>out</sub> with damping fitted)</td>
                    <td>{{ fmtHz(interaction.resonanceHz) }} · {{ fmtSi(interaction.characteristicImpedanceOhm, 'Ω') }}</td></tr>
                  <tr><td>Converter |Z<sub>in</sub>| = V²/P</td><td>{{ fmtSi(interaction.converterInputImpedanceOhm, 'Ω') }}</td></tr>
                  <tr><td>Margin (the strip chip; ≥ 12 dB comfortable, &lt; 6 dB add damping)</td>
                    <td :class="interaction.marginDb >= 12 ? 'pos' : interaction.marginDb >= 6 ? '' : 'neg'">
                      {{ fmtDb(interaction.marginDb) }} dB
                      <span class="note">R<sub>d</sub> = {{ fmtSi(interaction.dampingResistorOhm, 'Ω') }},
                      C<sub>d</sub> = {{ fmtSi(interaction.dampingCapacitorMinF, 'F') }}–{{ fmtSi(interaction.dampingCapacitorMaxF, 'F') }}</span></td></tr>
                  <tr><td class="note" colspan="2">Undamped, the LC section rings with a Q set by parasitic
                    resistances this tool does not know — the true undamped peak can be 10–50× R₀. Fit the
                    damping branch whenever this margin is what keeps the converter stable.</td></tr>
                </tbody>
              </table>
              </template>
            </template>

            <!-- SPICE netlist -->
            <template v-else-if="pane === 'netlist'">
              <div v-if="!design" class="pane-empty note">Design a filter first.</div>
              <template v-else>
              <p class="section-label">Filter + LISN in one deck — ready for Kirchhoff / ngspice / LTspice</p>
              <div class="row">
                <label class="field"><span>Excitation deck</span>
                  <select v-model="netlistMode" data-test="netlist-mode"
                          @change="api().then((e) => { try { netlist = e.filterSpiceNetlist(design, netlistLisn, netlistMode) } catch (deckError) { netlist = '* netlist unavailable: ' + deckError.message } })">
                    <option value="dm">differential-mode drive (C_X path)</option>
                    <option value="cm">common-mode drive (choke + Y caps)</option>
                  </select></label>
                <label class="field"><span>Artificial network</span>
                  <select v-model="netlistLisn" data-test="netlist-lisn"
                          @change="api().then((e) => { try { netlist = e.filterSpiceNetlist(design, netlistLisn, netlistMode) } catch (deckError) { netlist = '* netlist unavailable: ' + deckError.message } })">
                    <option value="cispr16">CISPR 16 — 50 µH mains LISN</option>
                    <option value="cispr25">CISPR 25 — 5 µH automotive LISN</option>
                  </select></label>
              </div>
              <p class="note">The LISN screen's network is embedded as the X subckt — the .ac
                sweep's vdb(measL)/vdb(measN) is literally what the receiver reads at the LISN
                measurement port, so this deck is the virtual conducted-emissions bench.</p>
              <pre class="code" data-test="netlist">{{ netlist }}</pre>
              <div class="row" style="margin-top: 0.6rem">
                <button class="ghost" @click="downloadNetlist">Download .cir</button>
                <button class="ghost" @click="copyNetlist">Copy</button>
              </div>
              </template>
            </template>

            <!-- LISN / test setup — reference view, works without a design -->
            <template v-else-if="pane === 'lisn'">
              <LisnView />
            </template>
          </div>
        </section>
      </div>
    </main>
  </div>
</template>
