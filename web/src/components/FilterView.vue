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
import { buildFilterCias, filterComponents, topologyLines } from '../ciasFilter.js'
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
const cxSource = ref('catalog')     // real parts by default; 'manual' opts out
const cxMfr = ref('')
const topology = ref('mains')   // 'mains' (1φ) | 'dc' (CISPR 25) | '3ph' (3-wire delta X) | '3phn' (3φ+N star X)
const nLines = computed(() => topologyLines(topology.value))
const eslXnH = ref(20)          // X-cap ESL (#290) — film-box typical; override from datasheet
const eslYnH = ref(10)          // per-Y-cap ESL; the parallel pair halves it
const lineCurrentA = ref(0)     // operating current for saturation screening (0 = off)
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
const asBuiltNote = ref('')     // set when the design was re-run with BOUND part values
const bindingSets = ref(null)   // receiver handoff: {cm: [[f,A]...], dm: [[f,A]...]}
const bindingNote = ref('')
const fCritCmHz = ref(null)     // per-mode critical design frequency from the binding sets
const fCritDmHz = ref(null)
const scanCtx = ref(null)       // the measured scan carried by the hand-off (#289/#291)

// ── output panes (Kirchhoff pattern: two, independently switchable) ──────────
const PANE_VIEWS = [
  ['schematic', 'SCHEMATIC'],
  ['parts', 'CATALOG PARTS'],
  ['bom', 'BOM & EXPORT'],
  ['il', 'INSERTION LOSS'],
  ['values', 'SIZING & SAFETY'],
  ['netlist', 'SPICE NETLIST'],
  ['result', 'PREDICTED RESULT'],
  ['lisn', 'TEST SETUP (LISN)'],
]
const paneA = ref('schematic')
const paneB = ref('il')
const openSection = ref('req')   // KH-style accordion: one input stage open at a time
const netlistLisn = ref('cispr16')   // which artificial network the SPICE deck embeds

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
  if (bindingSets.value && deriveFromBindings() && design.value) compute()
})
watch(topology, (t, previous) => {
  // DC: no mains, no touch-current story — auto-C_Y (a touch-budget concept)
  // and the 50 uH mains LISN both stop applying
  if (t === 'dc') {
    if (cYnF.value === 'auto') cYnF.value = 4.7
    netlistLisn.value = 'cispr25'
  } else {
    netlistLisn.value = 'cispr16'
  }
  // 3-phase grids are stated line-to-line: swap the DEFAULT voltage both ways,
  // but never overwrite a user-entered value
  const threePhase = (x) => x === '3ph' || x === '3phn'
  if (threePhase(t) && !threePhase(previous) && Number(gridVrms.value) === 230) gridVrms.value = 400
  if (!threePhase(t) && threePhase(previous) && Number(gridVrms.value) === 400) gridVrms.value = 230
})

// saturation screening (#290): datasheet comparison, not magnetics math —
// full L(I) derating stays in MKF per the house rule
function saturationWarn(p) {
  const i = Number(lineCurrentA.value)
  if (!(i > 0)) return null
  const peak = topology.value === 'dc' ? i : i * Math.SQRT2
  if (p.saturationCurrentPeakA != null && peak > p.saturationCurrentPeakA) {
    return `saturates: ${peak.toFixed(1)} A pk > ${p.saturationCurrentPeakA} A sat`
  }
  if (p.ratedCurrentA != null && i > p.ratedCurrentA) return `over ${p.ratedCurrentA} A rating`
  return null
}
const vInMin = ref(207)
const vInMinDirty = ref(false)
const pIn = ref(25)
const interaction = ref(null)
const lCmSource = ref('catalog')     // real parts by default; 'manual' opts out
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
  // a Y capacitor (and a touching person) sees phase-to-earth voltage;
  // 3-phase grids state vRms line-to-line
  const vTouch = Number(gridVrms.value) / (nLines.value >= 3 ? Math.sqrt(3) : 1)
  const budgetF = (Number(touchLimitMa.value) * 1e-3) /
    (vTouch * 1.1 * 2 * Math.PI * Number(gridHz.value) * 1.2 * Number(stages.value))
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

// catalog fetches are awaited by compute(): with catalog as the default
// source, computing before the fetch resolves must WAIT, never silently
// fall back to the manual list
let catalogFetch = null
let capsFetch = null
onMounted(() => {
  catalogFetch = (async () => {
    try {
      const response = await fetch('/kelvin/hertz-cmc.v1.json')
      if (!response.ok) throw new Error(String(response.status))
      catalog.value = await response.json()
      catalogState.value = 'ready'
    } catch {
      catalogState.value = 'unavailable'
    }
  })()
  capsFetch = (async () => {
    try {
      const response = await fetch('/kelvin/hertz-safety-caps.v1.json')
      if (response.ok) capsCatalog.value = await response.json()
    } catch { /* caps panes show their own unavailable state */ }
  })()
  onMountedHandoff()
})

function onMountedHandoff() {
  if (store.handoff) {
    scanCtx.value = store.handoff.scan ?? null
    if (store.handoff.binding) {
      bindingSets.value = store.handoff.binding
      if (!deriveFromBindings()) { store.handoff = null; return }
    } else {
      aReqCm.value = store.handoff.aReqCmDb ?? store.handoff.aReqDb
      aReqDm.value = store.handoff.aReqDmDb ?? store.handoff.aReqDb
      if (store.handoff.fSwHz) fSwKhz.value = Math.round(store.handoff.fSwHz / 1e3)
    }
    store.handoff = null
    // deliberately NOT computing: the hand-off fills the cards, the user walks
    // Requirement -> Components -> Grid & safety and presses DESIGN FILTER
  }
}

const manufacturers = () =>
  catalog.value ? [...new Set(catalog.value.parts.map((p) => p.manufacturer))].sort() : []

function catalogPartsAnyL() {
  if (!catalog.value) return []
  // an UNRATED part can never satisfy a positive current requirement — letting
  // null bypass the threshold put an 11.5 A SMD choke into a "100 A" design.
  // Set the threshold to 0 to browse unrated parts explicitly.
  const minRated = Number(minRatedA.value)
  // winding-count gate (#292): a 3-phase slot needs a 3/4-winding choke and a
  // 2-line slot must never offer one. Unknown winding count (null) is only
  // acceptable for 2-line topologies (most catalog rows predate the field).
  const windingOk = (p) => nLines.value >= 3
    ? p.windings === nLines.value
    : (p.windings == null || p.windings <= 2)
  return catalog.value.parts.filter((p) =>
    windingOk(p) &&
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
      // rec: WE-measured |Z| with Bode-reconstructed phase — shown with '~'
      column[part.mpn] = { db: il.standardDb[nearest], rec: !!cm.rec }
    }
    measuredIlAt.value = column
  } catch { /* column stays empty; parts remain selectable by value */ }
}

function catalogCandidates() {
  const parts = catalogParts()
  if (!parts.length) {
    const windingHint = nLines.value >= 3
      ? ` — no ${nLines.value}-winding (${nLines.value === 3 ? '3-phase' : '3-phase + N'}) chokes are catalogued; enter manual candidates from a 3-phase series datasheet (e.g. WE-CMB S)`
      : ''
    throw new Error('no catalog parts match the manufacturer/current/winding filters' + windingHint)
  }
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

function buildParams(overrides = {}) {
  const params = {
    fSwHz: fSwKhz.value * 1e3,
    aReqCmDb: Number(aReqCm.value),
    aReqDmDb: Number(aReqDm.value),
    cYPerLineF: resolvedCyNf() * 1e-9,
    stages: Number(stages.value),
    nLines: nLines.value,
    // #290: shunt-capacitor parasitics — the CM path sees the n-per-line Y
    // bank in parallel, dividing its ESL by n
    eslCmH: (Number(eslYnH.value) / nLines.value) * 1e-9,
    eslDmH: Number(eslXnH.value) * 1e-9,
    lCmCandidatesH: lCmSource.value === 'catalog'
      ? catalogCandidates() : parseList(lCandidatesMh.value, 1e-3),
    cXCandidatesF: cxSource.value === 'catalog'
      ? cxCatalogCandidates() : parseList(cxCandidatesUf.value, 1e-6),
  }
  if (topology.value !== 'dc') {
    // touch current / bleeder are a MAINS story (1-phase or 3-phase); a DC
    // supply filter has neither (chassis-referenced Y practice differs) —
    // omitting grid skips both in the engine instead of faking them
    params.grid = { vRms: Number(gridVrms.value), fHz: Number(gridHz.value), vSafe: 60, tDischargeS: 1 }
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
  return Object.assign(params, overrides)
}

async function compute() {
  asBuiltNote.value = ''
  await runDesign(() => buildParams(), { keepBindings: false })
}

async function runDesign(makeParams, { keepBindings }) {
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
    // catalog sources WAIT for their fetch; unreachable catalogs fail loudly
    if (lCmSource.value === 'catalog') {
      await catalogFetch
      if (catalogState.value !== 'ready') {
        throw new Error('the parts catalog is unreachable from this deployment — switch CM chokes to the manual value list')
      }
    }
    if (cxSource.value === 'catalog') {
      await capsFetch
      if (!capsCatalog.value) {
        throw new Error('the safety-caps catalog is unreachable from this deployment — switch X capacitors to the manual value list')
      }
    }
    const params = makeParams()
    if (!keepBindings) {
      bindings.value = {}
      selectedRef.value = ''
      measured.value = null
      measuredIlAt.value = {}
    }
    // The verdict criterion is the nominal in-circuit insertion loss (25 Ω CM /
    // 100 Ω DM — the terminations a CISPR 16 AMN actually presents). The ANP015
    // asymptote only SIZES; if the sized part misses the in-circuit criterion,
    // escalate through the candidate list until it passes or runs out.
    const evaluate = (p) => {
      const d = engine.designFilter(p)
      // the chip must be evaluated AT f_design — never snapped to a span edge
      const fLow = Math.min(d.fDesignCmHz, d.fDesignDmHz)
      const fHigh = Math.max(d.fDesignCmHz, d.fDesignDmHz)
      // span must also cover the carried scan so the residual view never
      // extrapolates the IL curves
      const scanF = (scanCtx.value?.traces ?? []).flatMap((t) => [t.frequenciesHz[0], t.frequenciesHz[t.frequenciesHz.length - 1]])
      const span = { fMinHz: Math.min(150e3, fLow / 2, ...scanF.filter((f) => f > 0)),
                     fMaxHz: Math.max(30e6, fHigh * 2, ...scanF), pointsPerDecade: 30 }
      // CM reference: all line LISN arms in parallel = 50/n Ω. DM stays 100 Ω
      // (a DM loop always runs out one arm and back another). The DM shunt is
      // the EFFECTIVE X capacitance (delta: 1.5·C per line pair).
      const cmRefZ = 50 / (d.nLines ?? 2)
      const cXEff = d.cXSelectedF * (d.cXDmFactor ?? 1)
      const cm = engine.insertionLossCurves({ inductanceH: d.lCmSelectedH, capacitanceF: d.cYgF,
        stages: d.stages, referenceImpedanceOhm: cmRefZ, capEslH: params.eslCmH, ...span })
      const dm = engine.insertionLossCurves({ inductanceH: d.lDmH, capacitanceF: cXEff,
        stages: d.stages, referenceImpedanceOhm: 100, capEslH: params.eslDmH, ...span })
      // the chip is a hard pass/fail input: evaluate it AT f_design via a
      // micro-span, not at the nearest 30-per-decade grid point (<=1.35 dB off)
      const exactAt = (inductanceH, capacitanceF, refZ, fDesignHz, eslH) => {
        const il = engine.insertionLossCurves({ inductanceH, capacitanceF, stages: d.stages,
          referenceImpedanceOhm: refZ, fMinHz: fDesignHz * 0.9995, fMaxHz: fDesignHz * 1.0005,
          pointsPerDecade: 20000, capEslH: eslH })
        const mid = Math.floor(il.frequenciesHz.length / 2)
        return { standard: il.standardDb[mid], worst: il.worstCaseDb[mid] }
      }
      return { d, cm, dm, at: { cm: exactAt(d.lCmSelectedH, d.cYgF, cmRefZ, d.fDesignCmHz, params.eslCmH),
                                dm: exactAt(d.lDmH, cXEff, 100, d.fDesignDmHz, params.eslDmH) } }
    }
    // in-circuit IL at exactly f_design — the same criterion the verdict chip
    // scores, so the escalation target and the verdict can never disagree
    const ilAtDesign = (inductanceH, capacitanceF, refZ, fDesignHz, nStages, eslH) => {
      const il = engine.insertionLossCurves({ inductanceH, capacitanceF, stages: nStages,
        referenceImpedanceOhm: refZ, fMinHz: fDesignHz * 0.9995, fMaxHz: fDesignHz * 1.0005,
        pointsPerDecade: 20000, capEslH: eslH })
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
            ilAtDesign(v, attempt.d.cYgF, 50 / (attempt.d.nLines ?? 2), attempt.d.fDesignCmHz, attempt.d.stages, params.eslCmH) >= Number(params.aReqCmDb))
          next.lCmCandidatesH = pass !== undefined
            ? params.lCmCandidatesH.filter((v) => v >= pass) : [larger[larger.length - 1]]
          changed = true
        }
      }
      if (needDm) {
        const larger = params.cXCandidatesF.filter((v) => v > attempt.d.cXSelectedF).sort((a, b) => a - b)
        if (larger.length) {
          const pass = larger.find((v) =>
            ilAtDesign(attempt.d.lDmH, v * (attempt.d.cXDmFactor ?? 1), 100, attempt.d.fDesignDmHz, attempt.d.stages, params.eslDmH) >= Number(params.aReqDmDb))
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
    roundTrip.value = null
    worstCaseAt.value = attempt.at
    const d = design.value
    interaction.value = engine.inputFilterInteraction(d.lDmH, d.cXSelectedF * (d.cXDmFactor ?? 1),
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
  ...(roundTrip.value?.cm ? [{ id: 'cmsp', label: 'CM ngspice round-trip (LISN bench)', color: 'var(--s-1)', dash: '9 3',
    points: roundTrip.value.cm.f.map((f, i) => ({ f, v: roundTrip.value.cm.il[i] })) }] : []),
  ...(roundTrip.value?.dm ? [{ id: 'dmsp', label: 'DM ngspice round-trip (LISN bench)', color: 'var(--s-2)', dash: '9 3',
    points: roundTrip.value.dm.f.map((f, i) => ({ f, v: roundTrip.value.dm.il[i] })) }] : []),
  ...(measured.value?.cm ? [{ id: 'cmm', label: `CM measured (${measured.value.mpn})`, color: 'var(--s-1)', dash: '1 4',
    points: measured.value.cm.f.map((f, i) => ({ f, v: measured.value.cm.db[i] })) }] : []),
  ...(measured.value?.dm ? [{ id: 'dmm', label: `DM measured (${measured.value.mpn})`, color: 'var(--s-2)', dash: '1 4',
    points: measured.value.dm.f.map((f, i) => ({ f, v: measured.value.dm.db[i] })) }] : []),
  { id: 'cm', label: `CM in-circuit (${(50 / nLines.value).toFixed(nLines.value === 3 ? 1 : 0)} Ω)`, color: 'var(--s-1)',
    points: ilCm.value.frequenciesHz.map((f, i) => ({ f, v: ilCm.value.standardDb[i] })) },
  { id: 'cmw', label: 'CM worst case (CISPR 17)', color: 'var(--s-1)', dash: '3 4',
    points: ilCm.value.frequenciesHz.map((f, i) => ({ f, v: ilCm.value.worstCaseDb[i] })) },
  { id: 'dm', label: 'DM in-circuit (100 Ω)', color: 'var(--s-2)',
    points: ilDm.value.frequenciesHz.map((f, i) => ({ f, v: ilDm.value.standardDb[i] })) },
  { id: 'dmw', label: 'DM worst case (CISPR 17)', color: 'var(--s-2)', dash: '3 4',
    points: ilDm.value.frequenciesHz.map((f, i) => ({ f, v: ilDm.value.worstCaseDb[i] })) },
]
// With a hand-off, EVERY binding point of the scan appears as a requirement
// marker (the failing points, each with its own local need); manual designs
// keep the two dots at f_design.
const requirementMarkers = () => {
  if (bindingSets.value) {
    const cap = 300
    return [
      ...(bindingSets.value.cm ?? []).slice(0, cap).map(([f, a]) => ({ f, v: a, color: 'var(--s-1)' })),
      ...(bindingSets.value.dm ?? []).slice(0, cap).map(([f, a]) => ({ f, v: a, color: 'var(--s-2)' })),
    ]
  }
  return [
    { f: design.value.fDesignCmHz, v: Number(aReqCm.value), color: 'var(--s-1)' },
    { f: design.value.fDesignDmHz, v: Number(aReqDm.value), color: 'var(--s-2)' },
  ]
}

const schematicLabels = () => {
  const labels = {}
  for (const c of filterComponents(design.value.stages, nLines.value)) {
    labels[c.ref] = c.kind === 'cmc' ? fmtSi(design.value.lCmSelectedH, 'H')
      : c.kind === 'cx' ? fmtSi(design.value.cXSelectedF, 'F')
      : fmtSi(design.value.cYPerLineF, 'F')
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
  // The caps manufacturer filter scopes the RECOMMENDED parts too — the value
  // candidates and the offered parts must come from the same pool, or a
  // Würth-filtered design recommends WIMA (closest-value sorting favors the
  // biggest manufacturer otherwise).
  const capsMfr = cxSource.value === 'catalog' ? cxMfr.value : ''
  pool = (capsCatalog.value?.parts ?? [])
    .filter((p) => p.safetyClass === cls && (!capsMfr || p.manufacturer === capsMfr))
    .map((p) => ({ ...p, valueF: p.capacitanceF }))
  if (!pool.length) {
    return { kind, target, parts: [], unavailable: !capsCatalog.value,
             filteredOut: !!capsCatalog.value, capsMfr }
  }
  // parallel BANKS (2-4 x the same part) compete with singles on deviation:
  // n x C hits capacitances no single part matches, and splits the ripple
  // current across the bank
  const banks = pool.flatMap((p) => [2, 3, 4].map((n) => ({
    ...p, quantity: n, valueF: n * p.capacitanceF, singleValueF: p.capacitanceF,
  })))
  const parts = [...pool.map((p) => ({ ...p, quantity: 1 })), ...banks]
    .map((p) => ({ ...p, deviation: Math.abs(p.valueF - target) / target }))
    .filter((p) => p.quantity === 1 || p.deviation < 0.25)   // banks only when they genuinely fit
    .sort((a, b) => a.deviation - b.deviation || a.quantity - b.quantity ||
      String(a.mpn).localeCompare(String(b.mpn)))
    .slice(0, 8)
  return { kind, target, parts, unavailable: false }
}

watch(selectedRef, (ref_) => {
  if (ref_ && kindOf(ref_) === 'cmc') loadMeasuredIlColumn()
})

async function bindPart(part) {
  const kind = kindOf(selectedRef.value)
  for (const c of filterComponents(design.value.stages, nLines.value)) {
    if (c.kind === kind) bindings.value[c.ref] = { mpn: part.mpn, manufacturer: part.manufacturer,
      valueF: part.valueF ?? null, quantity: part.quantity ?? 1,
      ratedVAcV: part.ratedVoltageAcV ?? null, ratedVDcV: part.ratedVoltageDcV ?? null,
      safetyClass: part.safetyClass ?? null }
  }
  bindings.value = { ...bindings.value }
  await recomputeAsBuilt()
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
    const result = { mpn: part.mpn, rec: !!(curve.cm?.rec || curve.dm?.rec) }
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
// IEC 60384-14 class ceilings: the class ITSELF caps the AC rating, so a
// class-marked part with no explicit AC figure is still checkable.
const SAFETY_CLASS_VAC = { X1: 500, X2: 310, Y1: 500, Y2: 300 }
const bindingVoltageWarning = (binding, kind) => {
  if (!binding) return null
  // what the position actually sees: a delta X capacitor sits line-to-line;
  // star/pair X and every Y capacitor see phase-to-earth (v_LL/sqrt(3) on
  // 3-phase grids).
  const vll = Number(gridVrms.value)
  const vPhase = nLines.value >= 3 ? vll / Math.sqrt(3) : vll
  const vSeen = kind === 'cx' && nLines.value === 3 ? vll : vPhase
  if (topology.value === 'dc') {
    // DC bus, no crest factor; an AC-rated film part withstands at least its
    // AC RMS figure as DC, so the AC rating is a conservative fallback basis
    const rated = binding.ratedVDcV ?? binding.ratedVAcV
    return rated != null && rated < vSeen
      ? `rated ${rated} V < ${Math.round(vSeen)} V bus — NOT rated for it` : null
  }
  // AC position: compare RMS against an AC-basis rating — a DC figure is NOT
  // an AC rating (recurring-peak/dV-dt stress), so it never silences the check
  const acRated = binding.ratedVAcV ?? SAFETY_CLASS_VAC[binding.safetyClass] ?? null
  if (acRated != null && acRated < vSeen) {
    const basis = binding.ratedVAcV != null ? `rated ${acRated} VAC`
      : `${binding.safetyClass} class (≤${acRated} VAC)`
    return `${basis} < ${Math.round(vSeen)} V RMS at this position — NOT rated for it`
  }
  if (acRated == null && binding.ratedVDcV != null && binding.ratedVDcV < vSeen * Math.SQRT2) {
    return `only a ${binding.ratedVDcV} V DC rating < ${Math.round(vSeen * Math.SQRT2)} V ` +
      'recurring peak — verify AC capability'
  }
  return null
}
const bomRows = () => filterComponents(design.value.stages, nLines.value).map((c) => ({
  ref: c.ref,
  value: schematicLabels()[c.ref],
  binding: bindings.value[c.ref] ?? null,
  warning: bindingVoltageWarning(bindings.value[c.ref], c.kind),
}))
const allBound = () => filterComponents(design.value.stages, nLines.value).every((c) => bindings.value[c.ref]?.mpn)

// Binding a part re-runs the WHOLE evaluation with the bound values — chips,
// curves, netlist, Middlebrook and safety all describe the filter AS BUILT,
// not the catalog-free sizing. Candidate lists are pinned to the bound
// values; a bound part too small for the requirement fails loudly.
async function recomputeAsBuilt() {
  if (!design.value) return
  const boundByKind = (kind) => {
    const c = filterComponents(design.value.stages, nLines.value).find((k) => k.kind === kind)
    return c ? bindings.value[c.ref] : null
  }
  const lCm = boundByKind('cmc')?.valueF ?? design.value.lCmSelectedH
  const cX = boundByKind('cx')?.valueF ?? design.value.cXSelectedF
  const cY = boundByKind('cy')?.valueF ?? design.value.cYPerLineF
  // loud but non-destructive: if the bound combination cannot meet the sizing
  // (e.g. a much smaller Y capacitor), keep the previous design on screen and
  // surface WHY the as-built rerun failed
  const snapshot = { design: design.value, ilCm: ilCm.value, ilDm: ilDm.value,
                     worstCaseAt: worstCaseAt.value, interaction: interaction.value,
                     netlist: netlist.value, escalated: escalated.value }
  await runDesign(() => buildParams({
    lCmCandidatesH: [lCm], cXCandidatesF: [cX], cYPerLineF: cY,
  }), { keepBindings: true })
  if (design.value) {
    asBuiltNote.value = 'AS BUILT — chips, curves, netlist and safety recomputed with the bound '
      + `parts: L_CM ${fmtSi(lCm, 'H')} · C_X ${fmtSi(cX, 'F')} · C_Y ${fmtSi(cY, 'F')}`
  } else {
    error.value = 'as-built rerun with the bound parts failed — showing the DESIGNED filter instead: ' + error.value
    design.value = snapshot.design
    ilCm.value = snapshot.ilCm
    ilDm.value = snapshot.ilDm
    worstCaseAt.value = snapshot.worstCaseAt
    interaction.value = snapshot.interaction
    netlist.value = snapshot.netlist
    escalated.value = snapshot.escalated
    asBuiltNote.value = ''
  }
}

// ── #289/#291: predicted post-filter residual against the scan's own limit ──
// Transfer-function route: predicted = measured − in-circuit IL(f) per mode.
// A plain line trace is bounded with the WEAKER mode's IL (whatever the mode
// mix, each component gets at least min(IL_cm, IL_dm)) — conservative and
// disclosed. Bound-part measured curves take precedence over ideal elements
// within their measured span.
function ilInterp(fHz, source) {
  const freqs = source.f, db = source.db
  if (fHz <= freqs[0]) return db[0]
  if (fHz >= freqs[freqs.length - 1]) return db[db.length - 1]
  let lo = 0
  let hi = freqs.length - 1
  while (hi - lo > 1) {
    const mid = (lo + hi) >> 1
    if (freqs[mid] <= fHz) lo = mid
    else hi = mid
  }
  const t = Math.log(fHz / freqs[lo]) / Math.log(freqs[hi] / freqs[lo])
  return db[lo] + t * (db[hi] - db[lo])
}
function ilSourceFor(mode) {
  const ideal = { cm: { f: ilCm.value.frequenciesHz, db: ilCm.value.standardDb },
                  dm: { f: ilDm.value.frequenciesHz, db: ilDm.value.standardDb } }
  const meas = { cm: measured.value?.cm, dm: measured.value?.dm }
  const one = (m) => (fHz) => {
    const mc = meas[m]
    if (mc && fHz >= mc.f[0] && fHz <= mc.f[mc.f.length - 1]) return ilInterp(fHz, mc)
    return ilInterp(fHz, ideal[m])
  }
  if (mode === 'cm') return one('cm')
  if (mode === 'dm') return one('dm')
  return (fHz) => Math.min(one('cm')(fHz), one('dm')(fHz))
}
// ── #299: ngspice round-trip — an INDEPENDENT engine (Kirchhoff's in-browser
// libngspice) solves the exported deck; the ABCD twin is evaluated under the
// deck's ACTUAL terminations (deckAbcdIl). IL = vdb_ref − vdb_filtered — the
// LISN's own eut→receiver divider cancels exactly in the ratio.
const roundTrip = ref(null)     // {cm:{f,il,twin}, dm:{...}, verdict:{cm:{spice,twin,delta},...}} | {error}
const roundTripBusy = ref(false)
let khEnginePromise = null
function kirchhoffEngine() {
  if (!khEnginePromise) {
    khEnginePromise = import(/* @vite-ignore */ new URL('/kirchhoff.js', window.location.origin).href)
      .then((m) => m.default())
  }
  return khEnginePromise
}
async function runRoundTrip() {
  if (!design.value) return
  roundTripBusy.value = true
  roundTrip.value = null
  try {
    const kh = await kirchhoffEngine()
    const engine = await api()
    const port = nLines.value === 2 ? 'meas_line' : 'meas_l1'
    const result = { verdict: {} }
    for (const mode of ['cm', 'dm']) {
      const filt = JSON.parse(kh.run_ngspice_ac(engine.filterSpiceNetlist(design.value, netlistLisn.value, mode)))
      const ref = JSON.parse(kh.run_ngspice_ac(engine.filterReferenceNetlist(netlistLisn.value, mode, nLines.value)))
      if (!filt.success) throw new Error(`ngspice (${mode} filter deck): ${filt.error}`)
      if (!ref.success) throw new Error(`ngspice (${mode} reference deck): ${ref.error}`)
      const keyF = Object.keys(filt.vectors).find((k) => k.toLowerCase() === port)
      const keyR = Object.keys(ref.vectors).find((k) => k.toLowerCase() === port)
      if (!keyF || !keyR) throw new Error(`measurement vector ${port} missing from the ngspice result`)
      const mag = (v, i) => Math.hypot(v.re[i], v.im?.[i] ?? 0)
      const f = filt.frequenciesHz
      const il = f.map((_, i) => 20 * Math.log10(mag(ref.vectors[keyR], i) / mag(filt.vectors[keyF], i)))
      const twin = engine.deckAbcdIl(design.value, netlistLisn.value, mode, f)
      const fD = mode === 'cm' ? design.value.fDesignCmHz : design.value.fDesignDmHz
      const fClamped = Math.min(Math.max(fD, f[0]), f[f.length - 1])
      const spiceAt = ilInterp(fClamped, { f, db: il })
      const twinAt = ilInterp(fClamped, { f, db: twin })
      result[mode] = { f, il, twin }
      result.verdict[mode] = { spice: spiceAt, twin: twinAt, delta: Math.abs(spiceAt - twinAt) }
    }
    roundTrip.value = result
  } catch (e) {
    roundTrip.value = { error: e.message }
  } finally {
    roundTripBusy.value = false
  }
}

const predicted = ref(null)   // {traces: [{name, mode, f[], measured[], predicted[]}], analysis, limitRuns}
async function computePredicted() {
  predicted.value = null
  if (!scanCtx.value || !design.value || !ilCm.value || !ilDm.value) return
  try {
    const engine = await api()
    const traces = scanCtx.value.traces.map((t) => {
      const il = ilSourceFor(t.mode)
      return { ...t, predicted: t.frequenciesHz.map((f, i) => t.levelsDbuv[i] - il(f)) }
    })
    // predicted verdict: the WORST trace decides, judged on the scan's limit
    let analysis = null
    for (const t of traces) {
      try {
        const a = engine.limitAnalysis(scanCtx.value.standardId, scanCtx.value.detector,
                                       t.frequenciesHz, t.predicted)
        if (!analysis || a.worst.marginDb < analysis.worst.marginDb) analysis = a
      } catch { /* trace entirely outside the limit — skip */ }
    }
    const fAll = traces.flatMap((t) => t.frequenciesHz)
    const limitRuns = engine.limitPolyline(scanCtx.value.standardId, scanCtx.value.detector,
                                           Math.min(...fAll), Math.max(...fAll))
    // the report also needs the MEASURED verdict: worst margin + offender rows
    const measuredOffenders = []
    let measuredWorst = null
    for (const t of traces) {
      try {
        const a = engine.limitAnalysis(scanCtx.value.standardId, scanCtx.value.detector,
                                       t.frequenciesHz, t.levelsDbuv)
        if (!measuredWorst || a.worst.marginDb < measuredWorst.marginDb) measuredWorst = a.worst
        a.marginsDb.forEach((margin, i) => {
          if (margin !== null && margin < 0) {
            measuredOffenders.push({ trace: t.name, mode: t.mode, f: t.frequenciesHz[i],
              level: t.levelsDbuv[i], limit: a.limitsDbuv[i], margin })
          }
        })
      } catch { /* trace entirely outside the limit — skip */ }
    }
    measuredOffenders.sort((a, b) => a.margin - b.margin)
    predicted.value = { traces, analysis, limitRuns,
      measured: { offenders: measuredOffenders.slice(0, 8), worst: measuredWorst } }
  } catch (e) {
    error.value = 'predicted result unavailable: ' + e.message
  }
}
watch([design, measured], () => { computePredicted() })
const predictedSeries = () => predicted.value.traces.flatMap((t, i) => [
  { id: `m${i}`, label: `${t.name} (measured)`, color: 'var(--ink-dim)', dash: '2 3',
    points: t.frequenciesHz.map((f, k) => ({ f, v: t.levelsDbuv[k] })) },
  { id: `p${i}`, label: `${t.name} — predicted with this filter`, color: i === 0 ? 'var(--s-1)' : 'var(--s-2)',
    points: t.frequenciesHz.map((f, k) => ({ f, v: t.predicted[k] })) },
])
const predictedRefRuns = () => (predicted.value.limitRuns ? [{
  label: 'limit', color: 'var(--s-limit)', dash: '7 5', runs: predicted.value.limitRuns.runs,
}] : [])
// A coupled pair cannot leak more than its full loop: K = 1 - L_dm/(2 L_cm)
// must stay in (0,1). The netlist already refuses; the design panel must not
// present an unbuildable BOM as valid either.
const unrealizable = () => design.value && design.value.lDmH >= 2 * design.value.lCmSelectedH

function downloadCias() {
  const brick = buildFilterCias(design.value.stages, bindings.value, topology.value)
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

// ── #293: printable pre-compliance report — the artifact handed to managers.
// Client-side only: a print-styled sheet with scan, verdict, coverage,
// design, BOM (real MPNs) and netlist; the browser's print dialog does the
// PDF. Nothing leaves the page.
const showReport = ref(false)
const showReportDialog = ref(false)
const reportProject = ref('')
const reportEut = ref('')
const reportAuthor = ref('')
function printReport() {
  showReportDialog.value = true
}
function confirmPrintReport() {
  showReportDialog.value = false
  showReport.value = true
  setTimeout(() => { window.print(); showReport.value = false }, 60)
}
const reportDate = () => new Date().toLocaleDateString('en-CA')   // local YYYY-MM-DD
const reportId = () => 'HZ-' + reportDate().replaceAll('-', '') + '-' +
  ((reportProject.value || 'design').toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '').slice(0, 24) || 'design')

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
      <button class="acc-head" :class="{ open: openSection === 'req' }" data-test="sec-req"
              @click="openSection = 'req'">1 · REQUIREMENT</button>
      <div v-show="openSection === 'req'" class="acc-body">
      <label class="field"><span>Topology</span>
        <select v-model="topology" data-test="topology">
          <option value="mains">single-phase mains — L/N/PE (CISPR 32/11)</option>
          <option value="dc">12/48 V DC supply — +/RTN/chassis (CISPR 25)</option>
          <option value="3ph">3-phase 3-wire — L1/L2/L3/PE, delta X</option>
          <option value="3phn">3-phase + N — L1/L2/L3/N/PE, star X</option>
        </select></label>
      <div class="row">
        <label class="field"><span>Switching frequency (kHz)</span>
          <input v-model.number="fSwKhz" type="number" min="1" data-test="fsw" @input="clearBinding" /></label>
        <label class="field"><span>Stages</span>
          <select v-model.number="stages" data-test="stages"><option :value="1">1 (40 dB/dec)</option><option :value="2">2 (80 dB/dec)</option></select></label>
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
      <button class="ghost cont" data-test="cont-req" @click="openSection = 'comp'">CONTINUE ▸ COMPONENTS</button>
      </div>

      <button class="acc-head" :class="{ open: openSection === 'comp' }" data-test="sec-comp"
              @click="openSection = 'comp'">2 · COMPONENTS</button>
      <div v-show="openSection === 'comp'" class="acc-body">
      <label class="field"><span title="Safety caps C_Y via the touch-current budget, never the spectrum. Within that cap a larger C_Y means a smaller choke — auto takes the largest standard value passing the tier selected under Grid & safety.">Y capacitor per line ⓘ</span>
        <select v-model="cYnF" data-test="cy-select">
          <option v-if="topology !== 'dc'" value="auto">auto — largest within the touch budget{{ autoCyNf !== null ? ` (→ ${autoCyNf} nF)` : '' }}</option>
          <option v-for="v in [1, 2.2, 3.3, 4.7, 10]" :key="v" :value="v">{{ v }} nF (manual)</option>
        </select></label>
      <label class="field"><span title="The DM path is sized from the choke's leakage inductance: one |Z| point from the datasheet's DM curve, several points (fitted, refused if they straddle the self-resonance), or the leakage value directly.">DM (leakage) inductance from ⓘ</span>
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
      <label class="field"><span title="The values the designer may select from: a manual list, or real parts from the TAS catalog filtered by manufacturer and rated current (a positive current threshold excludes unrated parts).">CM choke candidates ⓘ</span>
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
      <label class="field"><span title="A manual µF list, or real X2 safety capacitors from the catalog. The manufacturer filter scopes both the candidate values and the recommended parts (X2 and Y2).">X capacitor candidates ⓘ</span>
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
      <div class="row">
        <label class="field"><span title="Series inductance of the X capacitor (leads + body). Above the cap's self-resonance 1/(2π√(ESL·C)) the branch turns inductive and the real filter stops improving — film-box X2 typically 15–30 nH; take it from the datasheet.">X-cap ESL (nH) ⓘ</span>
          <input v-model.number="eslXnH" type="number" min="0" data-test="esl-x" /></label>
        <label class="field"><span title="Per-Y-capacitor series inductance; the parallel pair halves it in the CM path. Disc/film Y2 typically 5–15 nH.">Y-cap ESL (nH) ⓘ</span>
          <input v-model.number="eslYnH" type="number" min="0" data-test="esl-y" /></label>
      </div>
      <button class="ghost cont" data-test="cont-comp" @click="openSection = 'grid'">CONTINUE ▸ GRID &amp; SAFETY</button>
      </div>

      <button class="acc-head" :class="{ open: openSection === 'grid' }" data-test="sec-grid"
              @click="openSection = 'grid'">3 · GRID &amp; SAFETY</button>
      <div v-show="openSection === 'grid'" class="acc-body">
      <div class="row">
        <label class="field"><span>{{ topology === 'dc' ? 'Bus voltage (V DC)'
          : nLines >= 3 ? 'Grid (V line-to-line RMS)' : 'Grid (V RMS)' }}</span>
          <input v-model.number="gridVrms" type="number" /></label>
        <label v-if="topology !== 'dc'" class="field"><span>Grid (Hz)</span><input v-model.number="gridHz" type="number" /></label>
        <label class="field"><span title="Operating line/bus current for saturation screening: parts whose datasheet saturation current sits below the operating peak are flagged in the parts pane. 0 disables the check. Full L(I) derating via MKF is planned.">Line current (A) ⓘ</span>
          <input v-model.number="lineCurrentA" type="number" min="0" data-test="line-current" /></label>
      </div>
      <label v-if="topology !== 'dc'" class="field"><span>Touch-current tier (bounds C_Y)</span>
        <select v-model.number="touchLimitMa" data-test="touch-tier">
          <option :value="3.5">3.5 mA — IEC 62368-1 Class I</option>
          <option :value="0.75">0.75 mA — appliance</option>
          <option :value="0.5">0.5 mA — medical</option>
        </select></label>
      <p v-if="nLines >= 3" class="note">3-phase touch current is the conservative per-phase worst
        case: one Y set at phase-to-earth voltage (v<sub>LL</sub>/√3). Balanced-network vector
        cancellation is real but not credited — an unbalanced or single-faulted network loses it.</p>
      <p v-if="topology === 'dc'" class="note">DC/chassis topology: no touch-current budget and no
        discharge bleeder — C_Y is chosen by chassis-leakage and resonance practice, and the SPICE
        deck embeds the 5 µH CISPR 25 network.</p>
      <div class="row">
        <label class="field"><span>Converter V<sub>in</sub> min (V)</span>
          <input v-model.number="vInMin" type="number" @input="vInMinDirty = true" @change="compute" /></label>
        <label class="field"><span>Input power (W)</span>
          <input v-model.number="pIn" type="number" @change="compute" /></label>
      </div>
      <p class="note">V<sub>in</sub>/P feed the Middlebrook stability check; the tier bounds C_Y
        and scores the touch-current verdict.</p>
      <button class="act cont" data-test="compute" @click="compute">DESIGN FILTER</button>
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
          <span class="fcell"><b>C<sub>Y</sub></b><span>{{ nLines }}×{{ fmtSi(design.cYPerLineF, 'F') }}/stage</span></span>
          <span v-if="design.dischargeResistorOhm" class="fcell"><b>R<sub>bleed</sub></b>
            <span>{{ fmtSi(design.dischargeResistorOhm, 'Ω') }} · {{ fmtSi(design.dischargeResistorPowerW, 'W') }}</span></span>
        </div>
        <p v-if="unrealizable()" class="err" data-test="k-warning" style="margin: 0.35rem 0 0">
          Not realizable as drawn: the DM (leakage) inductance {{ fmtSi(design.lDmH, 'H') }} is
          ≥ 2 × the selected CM choke {{ fmtSi(design.lCmSelectedH, 'H') }} — a coupled pair cannot
          leak more than 2·L<sub>CM</sub> (coupling K would leave (0,1)). Enter a real leakage value
          or pick a larger choke; the CIAS export is disabled until this is resolved.</p>
        <p v-if="design.lCmFloorFromLeakage" class="note" data-test="leakage-floor-note" style="margin: 0.35rem 0 0">
          The CM requirement alone needed only {{ fmtSi(design.lCmRequiredH, 'H') }} — the choke
          selection was floored at L<sub>DM</sub>/2 because a coupled choke cannot leak more than
          2·L<sub>CM</sub> (assumed leakage {{ fmtSi(design.lDmH, 'H') }}). If your real part's
          leakage is smaller, enter it in 2 · COMPONENTS and re-design.</p>
        <button class="ghost report-btn" data-test="print-report" @click="printReport">PRINT REPORT</button>
        <p v-if="escalated" class="note" data-test="escalated-note" style="margin: 0.25rem 0 0">
          Asymptote-sized parts missed the in-circuit criterion — the selector escalated to larger
          candidates; the chips score what was actually selected.</p>
        <p v-if="asBuiltNote" class="note" data-test="as-built-note" style="margin: 0.25rem 0 0; color: var(--amber)">
          {{ asBuiltNote }}</p>
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
                                 :selected="selectedRef" :topology="topology" @select="selectComponent" />
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
                <p v-else-if="recommendations().filteredOut" class="note" data-test="parts-filtered-out">
                  No {{ kindOf(selectedRef) === 'cx' ? 'X2' : 'Y2' }} parts from
                  {{ recommendations().capsMfr }} in the catalog — loosen the manufacturer filter
                  under “X capacitor candidates”.</p>
                <table v-else class="data">
                  <thead><tr><th>Part</th><th>Manufacturer</th><th>Value</th>
                    <th v-if="kindOf(selectedRef) === 'cmc'">Meas. IL @ f<sub>design</sub></th>
                    <th v-if="kindOf(selectedRef) === 'cmc'">Rated V</th>
                    <th>{{ kindOf(selectedRef) === 'cmc' ? 'Rated A' : 'Class / V' }}</th><th></th></tr></thead>
                  <tbody>
                    <tr v-for="p in recommendations().parts" :key="p.mpn + '-' + (p.quantity ?? 1)">
                      <td><strong>{{ (p.quantity ?? 1) > 1 ? p.quantity + ' × ' : '' }}{{ p.mpn }}</strong>
                        <span v-if="(p.quantity ?? 1) > 1" class="note">parallel bank</span></td>
                      <td>{{ p.manufacturer }}</td>
                      <td><template v-if="p.valueF !== null">{{ fmtSi(p.valueF, kindOf(selectedRef) === 'cmc' ? 'H' : 'F') }}
                        <span v-if="p.deviation > 0.001" class="note">({{ (p.deviation * 100).toFixed(0) }}% off)</span></template>
                        <span v-else class="note">by measured curve</span></td>
                      <td v-if="kindOf(selectedRef) === 'cmc'" data-test="measured-il"
                          :class="measuredIlAt[p.mpn] !== undefined ? (measuredIlAt[p.mpn].db >= Number(aReqCm) ? 'pos' : 'neg') : ''">
                        {{ measuredIlAt[p.mpn] !== undefined
                          ? (measuredIlAt[p.mpn].rec ? '~' : '') + measuredIlAt[p.mpn].db.toFixed(1) + ' dB' : '—' }}</td>
                      <td v-if="kindOf(selectedRef) === 'cmc'" class="note">
                        {{ p.ratedVoltageAcV ? p.ratedVoltageAcV + ' VAC' : p.ratedVoltageDcV ? p.ratedVoltageDcV + ' VDC' : 'unrated — verify' }}</td>
                      <td>{{ kindOf(selectedRef) === 'cmc'
                        ? (p.ratedCurrentA !== null && p.ratedCurrentA !== undefined ? p.ratedCurrentA.toFixed(1) : 'unrated — verify')
                        : p.safetyClass + (p.ratedVoltageV ? ' / ' + p.ratedVoltageV + ' V*' : '') }}
                        <span v-if="kindOf(selectedRef) === 'cmc' && saturationWarn(p)"
                              style="color: var(--fault)" data-test="saturation-warn"> {{ saturationWarn(p) }}</span></td>
                      <td><button class="ghost" data-test="bind-part" @click="bindPart(p)">Use</button></td>
                    </tr>
                  </tbody>
                </table>
                <p v-if="kindOf(selectedRef) !== 'cmc'" class="note">Parallel-bank rows wire n
                  identical parts in parallel: the capacitances add, the ripple current splits, and
                  the CIAS export expands the bank into n components on the same nets.</p>
                <p v-if="kindOf(selectedRef) === 'cx' && nLines === 3" class="err" data-test="delta-x-voltage-note">
                  Delta X capacitors sit across the full {{ gridVrms }} V line-to-line voltage — the X2
                  class itself is rated ≤310 VAC, so NO catalogued safety cap qualifies as-is. Use
                  line-to-line-rated film (e.g. X1/440 VAC+), a series pair, or move the design to the
                  3-phase + N (star) topology where each X sees phase voltage; a bound X2 part is
                  flagged in the BOM.</p>
                <p v-if="kindOf(selectedRef) !== 'cmc'" class="note">*Datasheet rated voltage — MIXED
                  AC and DC bases: the X2/Y2 class itself is defined for ≤310 VAC mains; values like
                  630 V are DC ratings on the same film part. Verify the AC rating on the datasheet.</p>
                <p v-if="kindOf(selectedRef) === 'cmc'" class="note">
                  Most catalogued chokes carry no voltage rating — for mains use, verify insulation class
                  against the datasheet; chip-scale data-line chokes are never mains parts.
                  “Meas. IL” is computed from the part's impedance curve against your Y network at the
                  design frequency (SILENT-style selection). Unmarked values use measured complex Z
                  (Murata); values marked <strong>~</strong> use the manufacturer's measured |Z| with
                  phase reconstructed via the Bode gain-phase relation (WE via REDEXPERT — typically
                  within ±0.5 dB on this column, larger near resonance nulls). “By measured curve”
                  parts have no catalogued inductance — binding one keeps the designed values in the
                  netlist.</p>
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
                    <td v-if="row.binding"><strong>{{ (row.binding.quantity ?? 1) > 1 ? row.binding.quantity + ' × ' : '' }}{{ row.binding.mpn }}</strong>
                      <span class="note">{{ row.binding.manufacturer }}</span>
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
                <p style="display: flex; align-items: center; gap: 0.7rem; flex-wrap: wrap; margin: 0.4rem 0 0.1rem">
                  <button class="ghost" data-test="run-roundtrip" :disabled="roundTripBusy" @click="runRoundTrip">
                    {{ roundTripBusy ? 'RUNNING NGSPICE…' : 'NGSPICE ROUND-TRIP' }}</button>
                  <span v-if="roundTrip?.verdict" class="chip"
                        :class="roundTrip.verdict.cm.delta <= 1 && roundTrip.verdict.dm.delta <= 1 ? 'pass' : 'fail'"
                        data-test="roundtrip-verdict">
                    NGSPICE×ABCD CM Δ{{ roundTrip.verdict.cm.delta.toFixed(2) }} dB ·
                    DM Δ{{ roundTrip.verdict.dm.delta.toFixed(2) }} dB @ f<sub>design</sub></span>
                  <span v-if="roundTrip?.error" class="err" data-test="roundtrip-error">
                    ngspice round-trip failed: {{ roundTrip.error }}</span>
                </p>
                <p class="note">Independent cross-check (#299): the exported deck — this filter plus one
                  LISN per line — is solved by Kirchhoff's in-browser ngspice, and the ABCD model is
                  re-evaluated under the deck's own bench (≈0 Ω source, complex LISN load), ideal
                  elements as the deck states. The dashed round-trip curves are LISN-bench IL, not the
                  25/100 Ω chip quantities. Δ > 1 dB means the engines model DIFFERENT circuits and is
                  flagged, never averaged away. First run downloads the ~11 MB engine.</p>
                <p class="note">If the dashed worst-case curve still clears your requirement at the design
                  frequency, termination uncertainty cannot eat the margin. Ideal-element curves ignore
                  self-resonance and winding capacitance — above a few MHz a real single-stage filter
                  plateaus at 50–70 dB; bind a part with a measured curve (dotted) for the honest picture.</p>
                <p v-if="measured" class="note" data-test="measured-note">
                  Dotted: predicted with the <strong>measured impedance curve</strong> of {{ measured.mpn }}
                  (manufacturer data via the TAS catalog) — shown only over the measured span.
                  <template v-if="measured.rec">Phase reconstructed from measured |Z| via the Bode
                  gain-phase relation (validated on 219 Murata parts: 0.03 dB median / 0.5 dB p90 on
                  these curves; trust it less right at resonance nulls).</template></p>
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

            <!-- predicted post-filter residual (#289/#291) -->
            <template v-else-if="pane === 'result'">
              <div v-if="!scanCtx" class="pane-empty note">Bring a measurement over first —
                <em>Design the fix</em> (Spectrum) or <em>Design filter for these modes</em>
                (Receiver) carries the scan along for this comparison.</div>
              <div v-else-if="!design" class="pane-empty note">Design a filter first.</div>
              <div v-else-if="predicted">
                <p class="section-label">Measured scan vs predicted post-filter residual — the budget view</p>
                <div class="fstrip-row" style="margin-bottom: 0.4rem">
                  <span v-if="predicted.analysis" class="chip"
                        :class="predicted.analysis.worst.marginDb >= 0 ? 'pass' : 'fail'" data-test="predicted-verdict">
                    PREDICTED {{ predicted.analysis.worst.marginDb >= 0 ? 'PASS' : 'FAIL' }} ·
                    {{ fmtDb(predicted.analysis.worst.marginDb) }} dB @ {{ fmtHz(predicted.analysis.worst.frequencyHz) }}</span>
                </div>
                <LogChart :series="predictedSeries()" :ref-runs="predictedRefRuns()"
                          y-label="dBµV" :height="260" data-test="predicted-chart" />
                <p class="note">Transfer-function prediction: the measured trace minus this filter's
                  in-circuit insertion loss per mode (line traces are bounded with the WEAKER mode's
                  IL — conservative). Bound-part measured curves are used inside their span. Mode
                  conversion, layout parasitics and source-impedance deviation are NOT modeled —
                  verify with the SPICE deck and on the bench.</p>
              </div>
            </template>

            <!-- LISN / test setup — reference view, works without a design -->
            <template v-else-if="pane === 'lisn'">
              <LisnView />
            </template>
          </div>
        </section>
      </div>
    </main>

    <!-- ── report metadata dialog ─────────────────────────────────────────── -->
    <div v-if="showReportDialog" class="report-backdrop" data-test="report-dialog" @click.self="showReportDialog = false">
      <div class="report-dialog panel-hi">
        <p class="section-label">EMC PRE-COMPLIANCE REPORT</p>
        <label class="field"><span>Project</span>
          <input v-model="reportProject" data-test="report-project" placeholder="e.g. 65 W adapter, rev B" /></label>
        <label class="field"><span>Equipment under test</span>
          <input v-model="reportEut" data-test="report-eut" placeholder="e.g. QR flyback PSU, 300 kHz" /></label>
        <label class="field"><span>Author / engineer</span>
          <input v-model="reportAuthor" data-test="report-author" /></label>
        <p class="note">Generated and printed locally (browser print → PDF). Nothing leaves this machine.</p>
        <div style="display: flex; gap: 0.6rem; justify-content: flex-end; margin-top: 0.5rem">
          <button class="ghost" data-test="report-cancel" @click="showReportDialog = false">CANCEL</button>
          <button class="ghost" data-test="report-print-confirm" @click="confirmPrintReport">PRINT REPORT</button>
        </div>
      </div>
    </div>

    <!-- ── print-only report sheet (#293, professional layout) ────────────── -->
    <div v-if="design" class="report-sheet" :class="{ 'report-visible': showReport }" data-test="report-sheet">
      <h1>EMC pre-compliance report</h1>
      <table class="rep-id">
        <tbody>
          <tr><th>Project</th><td>{{ reportProject || '—' }}</td><th>Date</th><td>{{ reportDate() }}</td></tr>
          <tr><th>EUT</th><td>{{ reportEut || '—' }}</td><th>Report ID</th><td>{{ reportId() }}</td></tr>
          <tr><th>Author</th><td>{{ reportAuthor || '—' }}</td><th>Tool</th><td>Hertz engine v0.1.0 · hertz.openconverters.com</td></tr>
        </tbody>
      </table>
      <p class="rep-disclaimer">Pre-compliance estimates with engineering margins — <b>NOT a certification</b>.
        Results derive from transfer-function models and catalogued part data; mode conversion and layout
        parasitics are not modeled. Accredited chamber testing remains mandatory for a compliance claim.</p>

      <section>
        <h2>Summary of verdicts</h2>
        <table>
          <thead><tr><th>Check</th><th>Result</th><th>Value</th><th>Criterion</th></tr></thead>
          <tbody>
            <tr v-if="worstCaseAt">
              <td>CM in-circuit attenuation at f<sub>design</sub></td>
              <td :class="worstCaseAt.cm.standard >= Number(aReqCm) ? 'rep-pass' : 'rep-fail'">
                {{ worstCaseAt.cm.standard >= Number(aReqCm) ? 'PASS' : 'FAIL' }}</td>
              <td>{{ fmtDb(worstCaseAt.cm.standard) }} dB ({{ fmtDb(worstCaseAt.cm.worst) }} dB CISPR 17 worst case)</td>
              <td>≥ {{ fmtDb(Number(aReqCm), 0) }} dB</td></tr>
            <tr v-if="worstCaseAt">
              <td>DM in-circuit attenuation at f<sub>design</sub></td>
              <td :class="worstCaseAt.dm.standard >= Number(aReqDm) ? 'rep-pass' : 'rep-fail'">
                {{ worstCaseAt.dm.standard >= Number(aReqDm) ? 'PASS' : 'FAIL' }}</td>
              <td>{{ fmtDb(worstCaseAt.dm.standard) }} dB ({{ fmtDb(worstCaseAt.dm.worst) }} dB CISPR 17 worst case)</td>
              <td>≥ {{ fmtDb(Number(aReqDm), 0) }} dB</td></tr>
            <tr v-if="interaction">
              <td>Input-filter stability (Middlebrook)</td>
              <td :class="interaction.marginDb >= 6 ? 'rep-pass' : 'rep-fail'">
                {{ interaction.marginDb >= 12 ? 'PASS' : interaction.marginDb >= 6 ? 'MARGINAL' : 'FAIL' }}</td>
              <td>{{ fmtDb(interaction.marginDb) }} dB margin</td>
              <td>≥ 12 dB comfortable, ≥ 6 dB minimum</td></tr>
            <tr v-if="design.leakageCurrentA !== undefined">
              <td>Touch current (IEC 60990 model, worst-case tolerances)</td>
              <td :class="design.leakageCurrentA < touchLimitMa * 1e-3 ? 'rep-pass' : 'rep-fail'">
                {{ design.leakageCurrentA < touchLimitMa * 1e-3 ? 'PASS' : 'FAIL' }}</td>
              <td>{{ fmtSi(design.leakageCurrentA, 'A') }}</td>
              <td>&lt; {{ touchLimitMa }} mA tier</td></tr>
            <tr v-if="predicted && predicted.analysis">
              <td>Predicted post-filter emissions vs {{ scanCtx.standardId }}</td>
              <td :class="predicted.analysis.worst.marginDb >= 0 ? 'rep-pass' : 'rep-fail'">
                {{ predicted.analysis.worst.marginDb >= 0 ? 'PASS' : 'FAIL' }}</td>
              <td>{{ fmtDb(predicted.analysis.worst.marginDb) }} dB at {{ fmtHz(predicted.analysis.worst.frequencyHz) }}</td>
              <td>margin ≥ 0 dB at every measured point</td></tr>
          </tbody>
        </table>
        <p v-if="escalated" class="rep-meta">The asymptote-sized parts missed the in-circuit criterion —
          the selector escalated to larger catalog candidates; all verdicts score what was actually selected.</p>
        <p v-if="asBuiltNote" class="rep-meta">{{ asBuiltNote }}.</p>
      </section>

      <section v-if="scanCtx">
        <h2>Measurement</h2>
        <p>Judged against <b>{{ scanCtx.standardId }}</b> with the {{ scanCtx.detector.replace('_', '-') }} detector.
          Traces: {{ scanCtx.traces.map((t) => `${t.name} [${t.mode.toUpperCase()}]`).join(' · ') }}.
          <template v-if="predicted && predicted.measured && predicted.measured.worst">Worst measured margin
          {{ fmtDb(predicted.measured.worst.marginDb) }} dB at {{ fmtHz(predicted.measured.worst.frequencyHz) }}.</template></p>
        <p v-if="bindingNote" class="rep-meta">{{ bindingNote }}</p>
        <table v-if="predicted && predicted.measured && predicted.measured.offenders.length">
          <thead><tr><th>Trace</th><th>Frequency</th><th>Level (dBµV)</th><th>Limit (dBµV)</th><th>Margin (dB)</th></tr></thead>
          <tbody>
            <tr v-for="(o, i) in predicted.measured.offenders" :key="i">
              <td>{{ o.trace }} [{{ o.mode.toUpperCase() }}]</td><td>{{ fmtHz(o.f) }}</td>
              <td>{{ fmtDb(o.level) }}</td><td>{{ fmtDb(o.limit) }}</td>
              <td class="rep-fail">{{ fmtDb(o.margin) }}</td></tr>
          </tbody>
        </table>
      </section>

      <section>
        <h2>Filter design</h2>
        <p>
          Topology: {{ topology === 'dc' ? 'DC supply (+/RTN/chassis, CISPR 25 5 µH bench)'
            : topology === '3ph' ? '3-phase 3-wire (delta X capacitors), grid stated line-to-line'
            : topology === '3phn' ? '3-phase + neutral (star X capacitors), grid stated line-to-line'
            : 'single-phase L/N/PE (CISPR 16 50 µH bench)' }} · {{ design.stages }} stage(s).
        </p>
        <table>
          <thead><tr><th>Mode</th><th>f<sub>design</sub></th><th>Required</th><th>f<sub>co</sub></th><th>Components</th><th>Asymptotic</th></tr></thead>
          <tbody>
            <tr><td>Common mode</td><td>{{ fmtHz(design.fDesignCmHz) }}</td>
              <td>{{ fmtDb(Number(aReqCm), 0) }} dB</td><td>{{ fmtHz(design.fCutoffCmHz) }}</td>
              <td>L<sub>CM</sub> {{ fmtSi(design.lCmSelectedH, 'H') }} · {{ nLines }}×C<sub>Y</sub> {{ fmtSi(design.cYPerLineF, 'F') }}/stage</td>
              <td>{{ fmtDb(design.attenuationCmDb) }} dB</td></tr>
            <tr><td>Differential mode</td><td>{{ fmtHz(design.fDesignDmHz) }}</td>
              <td>{{ fmtDb(Number(aReqDm), 0) }} dB</td><td>{{ fmtHz(design.fCutoffDmHz) }}</td>
              <td>L<sub>DM</sub> {{ fmtSi(design.lDmH, 'H') }} (leakage) · C<sub>X</sub> {{ fmtSi(design.cXSelectedF, 'F') }}{{
                (design.cXDmFactor ?? 1) !== 1 ? ` (delta: pair sees ${fmtSi(design.cXSelectedF * design.cXDmFactor, 'F')})` : '' }}</td>
              <td>{{ fmtDb(design.attenuationDmDb) }} dB</td></tr>
          </tbody>
        </table>
        <p v-if="design.dischargeResistorOhm" class="rep-meta">X-capacitor discharge:
          R<sub>bleed</sub> {{ fmtSi(design.dischargeResistorOhm, 'Ω') }} ({{ fmtSi(design.dischargeResistorPowerW, 'W') }}
          continuous), sized for {{ topology === '3ph' ? 'line-to-line' : 'phase' }} voltage, V+10 % / C+20 % worst case.</p>
        <p v-if="nLines >= 3" class="rep-meta">Touch current is the conservative per-phase worst case
          (one Y set at v<sub>LL</sub>/√3); balanced-network vector cancellation is not credited.</p>
      </section>

      <section>
        <h2>Schematic</h2>
        <FilterSchematic :stages="design.stages" :labels="schematicLabels()" :bindings="bindings"
                         selected="" :topology="topology" :interactive="false" @select="() => {}" />
      </section>

      <section>
        <h2>In-circuit insertion loss</h2>
        <p class="rep-meta">Solid: nominal terminations (CM {{ (50 / nLines).toFixed(nLines === 3 ? 1 : 0) }} Ω,
          DM 100 Ω). Dashed: CISPR 17 approximate worst case (0.1 Ω ↔ 100 Ω both directions, smaller IL kept).
          Dots: requirement points.</p>
        <LogChart :series="ilSeries()" :violations="requirementMarkers()"
                  violation-label="requirements (CM green, DM blue)" y-label="dB" :height="230" />
      </section>

      <section v-if="predicted && predicted.analysis">
        <h2>Predicted post-filter result</h2>
        <p class="rep-meta">Measured trace minus this filter's per-mode in-circuit insertion loss
          (bound parts' measured curves inside their span; line traces bounded by the weaker mode —
          conservative). Worst predicted margin {{ fmtDb(predicted.analysis.worst.marginDb) }} dB at
          {{ fmtHz(predicted.analysis.worst.frequencyHz) }}.</p>
        <LogChart :series="predictedSeries()" :ref-runs="predictedRefRuns()" y-label="dBµV" :height="230" />
      </section>

      <section>
        <h2>Bill of materials</h2>
        <table>
          <thead><tr><th>Ref</th><th>Value</th><th>Part</th><th>Manufacturer</th><th>Rating</th><th>Flag</th></tr></thead>
          <tbody>
            <tr v-for="row in bomRows()" :key="row.ref">
              <td>{{ row.ref }}</td><td>{{ row.value }}</td>
              <td>{{ row.binding ? ((row.binding.quantity ?? 1) > 1 ? row.binding.quantity + ' × ' : '') + row.binding.mpn : 'unbound' }}</td>
              <td>{{ row.binding ? row.binding.manufacturer : '—' }}</td>
              <td>{{ row.binding ? [row.binding.safetyClass,
                     row.binding.ratedVAcV ? row.binding.ratedVAcV + ' VAC' : null,
                     row.binding.ratedVDcV ? row.binding.ratedVDcV + ' VDC' : null]
                     .filter(Boolean).join(' / ') || '—' : '—' }}</td>
              <td :class="row.warning ? 'rep-fail' : ''">{{ row.warning || '—' }}</td></tr>
          </tbody>
        </table>
      </section>

      <section>
        <h2>Method &amp; assumptions</h2>
        <ul class="rep-method">
          <li>Filter sizing per Würth Elektronik application note ANP015 (min-f<sub>co</sub> per mode,
            40·n dB/dec asymptotes), generalized over the line count; components rounded up onto the
            available candidate list, then verified in-circuit and escalated if short.</li>
          <li>In-circuit insertion loss from two-port ABCD chains between real terminations, including
            the CISPR 17 approximate worst case (0.1 Ω/100 Ω both directions). Shunt-capacitor ESL/ESR
            included where entered; the SPICE deck notes what it omits.</li>
          <li>Bound parts use catalogued data: manufacturer datasheets/parametrics, measured impedance
            curves where available (Murata: measured complex Z; Würth Elektronik: REDEXPERT |Z| with
            Bode gain-phase-reconstructed phase, marked '~', validated to 0.03 dB median on this quantity).</li>
          <li>Voltage flags compare each position's actual stress (delta X: line-to-line; star/pair X
            and Y: phase-to-earth; DC bus: no crest factor) against AC-basis ratings — a DC-only figure
            never silences an AC position; class ceilings per IEC 60384-14 (X2 ≤ 310 / Y2 ≤ 300 VAC).</li>
          <li>Conducted scope only ({{ scanCtx ? scanCtx.standardId : 'CISPR 32/25' }} conducted limits);
            the radiated screen provides a separate ±20 dB triage estimate, not reported here.</li>
        </ul>
        <p class="rep-meta">References: CISPR 32:2015+A1, CISPR 25:2021, CISPR 16-1-1, CISPR 17,
          IEC 60384-14, IEC 60990, WE ANP015. Generated locally in the browser; no data left this machine.</p>
      </section>

      <section>
        <h2>Appendix — SPICE deck (filter + LISN)</h2>
        <pre>{{ netlist }}</pre>
      </section>
    </div>
  </div>
</template>
