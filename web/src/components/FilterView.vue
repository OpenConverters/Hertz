<script setup>
// ANP015 line-filter designer: attenuation target in, component values out,
// with the safety checks (Y-cap leakage, X-cap discharge) and SPICE export.
import { onMounted, ref, watch } from 'vue'
import LogChart from './LogChart.vue'
import FilterSchematic from './FilterSchematic.vue'
import { api } from '../engine.js'
import { buildFilterCias, filterComponents } from '../ciasFilter.js'
import { store } from '../store.js'
import { fmtHz, fmtDb, fmtSi } from '../format.js'

const fSwKhz = ref(300)
const aReqCm = ref(40)
const aReqDm = ref(40)
const cYnF = ref(4.7)
const stages = ref(1)
const dmMode = ref('impedance')     // 'impedance' | 'inductance'
const dmImpedanceOhm = ref(92)
const dmImpedanceMhz = ref(1)
const lDmUh = ref(14.6)
const lCandidatesMh = ref('0.47, 0.68, 1, 1.5, 2.2, 3.3, 4.7, 6.8, 10')
const cxCandidatesUf = ref('0.1, 0.15, 0.22, 0.33, 0.47, 0.68, 1, 1.5, 2.2, 3.3')
const gridVrms = ref(230)
const gridHz = ref(50)
const touchLimitMa = ref(3.5)   // selected compliance tier for the touch-current verdict
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

// Reduce the binding sets to the (f*, A*) whose f/10^(A/(40 n)) is the global
// minimum — the designer's own cutoff formula then meets EVERY binding point.
function deriveFromBindings() {
  const n = Number(stages.value)
  const all = [...(bindingSets.value.cm ?? []), ...(bindingSets.value.dm ?? [])]
  if (!all.length) return
  let best = all[0]
  for (const point of all) {
    if (point[0] / 10 ** (point[1] / (40 * n)) < best[0] / 10 ** (best[1] / (40 * n))) best = point
  }
  fSwKhz.value = best[0] / 1e3
  aReqCm.value = Math.ceil(best[1])
  aReqDm.value = Math.ceil(best[1])
  bindingNote.value = `sized by the min-f_co rule over ${all.length} binding points ` +
    `(critical: ${Math.ceil(best[1])} dB @ ${(best[0] / 1e3).toFixed(0)} kHz, ${n}-stage)`
}

watch(stages, () => {
  if (bindingSets.value) { deriveFromBindings(); compute() }
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
const measuredIlAt = ref({})     // mpn -> measured CM IL (dB) at f_design, for the parts panel

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
  } catch { /* caps panel shows its own unavailable state */ }
  if (store.handoff) {
    if (store.handoff.binding) {
      bindingSets.value = store.handoff.binding
      deriveFromBindings()
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
  return catalog.value.parts.filter((p) =>
    (!mfrFilter.value || p.manufacturer === mfrFilter.value) &&
    (p.ratedCurrentA === null || p.ratedCurrentA >= Number(minRatedA.value)))
}

function catalogParts() {
  return catalogPartsAnyL().filter((p) => p.inductanceH > 0)
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
    const fDesign = design.value.fDesignHz
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

async function compute() {
  error.value = ''
  design.value = null
  netlist.value = ''
  escalated.value = false
  interaction.value = null
  ilCm.value = null
  ilDm.value = null
  worstCaseAt.value = null
  try {
    const engine = await api()
    if (lCmSource.value === 'catalog' && catalogState.value === 'ready' && !catalogCandidates().length) {
      throw new Error('the manufacturer / rated-current filters removed every catalog part — loosen them')
    }
    const params = {
      fSwHz: fSwKhz.value * 1e3,
      aReqCmDb: Number(aReqCm.value),
      aReqDmDb: Number(aReqDm.value),
      cYPerLineF: cYnF.value * 1e-9,
      stages: Number(stages.value),
      lCmCandidatesH: lCmSource.value === 'catalog' && catalogState.value === 'ready'
        ? catalogCandidates() : parseList(lCandidatesMh.value, 1e-3),
      cXCandidatesF: parseList(cxCandidatesUf.value, 1e-6),
      grid: { vRms: Number(gridVrms.value), fHz: Number(gridHz.value), vSafe: 60, tDischargeS: 1 },
    }
    if (dmMode.value === 'impedance') {
      params.dmImpedanceOhm = Number(dmImpedanceOhm.value)
      params.dmImpedanceFrequencyHz = dmImpedanceMhz.value * 1e6
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
      const span = { fMinHz: Math.min(150e3, d.fDesignHz / 2),
                     fMaxHz: Math.max(30e6, d.fDesignHz * 2), pointsPerDecade: 30 }
      const cm = engine.insertionLossCurves({ inductanceH: d.lCmSelectedH, capacitanceF: d.cYgF,
        stages: d.stages, referenceImpedanceOhm: 25, ...span })
      const dm = engine.insertionLossCurves({ inductanceH: d.lDmH, capacitanceF: d.cXSelectedF,
        stages: d.stages, referenceImpedanceOhm: 100, ...span })
      // the chip is a hard pass/fail input: evaluate it AT f_design via a
      // micro-span, not at the nearest 30-per-decade grid point (<=1.35 dB off)
      const exactAt = (inductanceH, capacitanceF, refZ) => {
        const il = engine.insertionLossCurves({ inductanceH, capacitanceF, stages: d.stages,
          referenceImpedanceOhm: refZ, fMinHz: d.fDesignHz * 0.9995, fMaxHz: d.fDesignHz * 1.0005,
          pointsPerDecade: 20000 })
        const mid = Math.floor(il.frequenciesHz.length / 2)
        return { standard: il.standardDb[mid], worst: il.worstCaseDb[mid] }
      }
      return { d, cm, dm, at: { cm: exactAt(d.lCmSelectedH, d.cYgF, 25),
                                dm: exactAt(d.lDmH, d.cXSelectedF, 100) } }
    }
    let attempt = evaluate(params)
    for (let guard = 0; guard < 8; guard += 1) {
      const needCm = attempt.at.cm.standard < Number(params.aReqCmDb)
      const needDm = attempt.at.dm.standard < Number(params.aReqDmDb)
      if (!needCm && !needDm) break
      const next = { ...params }
      let changed = false
      if (needCm) {
        const larger = params.lCmCandidatesH.filter((v) => v > attempt.d.lCmSelectedH)
        if (larger.length) { next.lCmCandidatesH = larger; changed = true }
      }
      if (needDm) {
        const larger = params.cXCandidatesF.filter((v) => v > attempt.d.cXSelectedF)
        if (larger.length) { next.cXCandidatesF = larger; changed = true }
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
      netlist.value = engine.filterSpiceNetlist(design.value, 'cispr16', netlistMode.value)
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
  { f: design.value.fDesignHz, v: Number(aReqCm.value), color: 'var(--s-1)' },
  { f: design.value.fDesignHz, v: Number(aReqDm.value), color: 'var(--s-2)' },
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
  <div class="grid2">
    <div>
      <div class="panel">
        <p class="section-label">Requirement</p>
        <div class="row">
          <label class="field"><span>Switching frequency (kHz)</span>
            <input v-model.number="fSwKhz" type="number" min="1" data-test="fsw" /></label>
          <label class="field"><span>Stages</span>
            <select v-model.number="stages"><option :value="1">1 (40 dB/dec)</option><option :value="2">2 (80 dB/dec)</option></select></label>
        </div>
        <div class="row">
          <label class="field"><span>Required CM attenuation (dB)</span>
            <input v-model.number="aReqCm" type="number" data-test="areq-cm" /></label>
          <label class="field"><span>Required DM attenuation (dB)</span>
            <input v-model.number="aReqDm" type="number" data-test="areq-dm" /></label>
        </div>
        <p class="note">Below 150 kHz the design moves to the first harmonic inside the measured band, as ANP015 prescribes.</p>
        <p v-if="bindingNote" class="note" data-test="binding-note">{{ bindingNote }}</p>
      </div>

      <div class="panel">
        <p class="section-label">Components</p>
        <label class="field"><span>Y capacitor per line (nF) — bounded by leakage current</span>
          <select v-model.number="cYnF"><option>1</option><option>2.2</option><option>3.3</option><option>4.7</option><option>10</option></select></label>
        <label class="field"><span>DM inductance source</span>
          <select v-model="dmMode">
            <option value="impedance">from choke DM impedance curve (Z at f)</option>
            <option value="inductance">total loop leakage inductance directly</option>
          </select></label>
        <div v-if="dmMode === 'impedance'" class="row">
          <label class="field"><span>|Z| (Ω)</span><input v-model.number="dmImpedanceOhm" type="number" /></label>
          <label class="field"><span>at (MHz)</span><input v-model.number="dmImpedanceMhz" type="number" /></label>
        </div>
        <label v-else class="field"><span>Total loop leakage (µH) — line+neutral in series opposition (≈ 2× per-winding)</span>
          <input v-model.number="lDmUh" type="number" /></label>
        <label class="field"><span>CM choke source</span>
          <select v-model="lCmSource" data-test="lcm-source">
            <option value="manual">manual value list</option>
            <option value="catalog" :disabled="catalogState !== 'ready'">
              parts catalog{{ catalogState === 'ready' ? ` (${catalog.count} chokes)` : catalogState === 'loading' ? ' (loading…)' : ' (unavailable here)' }}
            </option>
          </select></label>
        <label v-if="lCmSource === 'manual'" class="field"><span>CM choke candidates (mH) — any manufacturer</span>
          <input v-model="lCandidatesMh" type="text" /></label>
        <div v-else class="row">
          <label class="field"><span>Manufacturer</span>
            <select v-model="mfrFilter" data-test="mfr-filter"><option value="">all manufacturers</option>
              <option v-for="m in manufacturers()" :key="m" :value="m">{{ m }}</option></select></label>
          <label class="field"><span>Min. rated current (A)</span>
            <input v-model.number="minRatedA" type="number" min="0" data-test="min-rated" /></label>
        </div>
        <label class="field"><span>X capacitor candidates (µF)</span>
          <input v-model="cxCandidatesUf" type="text" /></label>
        <div class="row">
          <label class="field"><span>Grid voltage (V RMS)</span><input v-model.number="gridVrms" type="number" /></label>
          <label class="field"><span>Grid frequency (Hz)</span><input v-model.number="gridHz" type="number" /></label>
        </div>
        <button class="act" data-test="compute" @click="compute">Design filter</button>
      </div>

      <div v-if="error" class="err" data-test="error">{{ error }}</div>
    </div>

    <div v-if="design">
      <div class="panel panel-hi">
        <div class="readout">
          <div class="cell"><b>Design frequency</b><span>{{ fmtHz(design.fDesignHz) }}</span></div>
          <div class="cell"><b>Target cutoff</b><span>{{ fmtHz(design.fCutoffTargetHz) }}</span></div>
          <div class="cell"><b>Stages</b><span>{{ design.stages }}</span></div>
          <div v-if="worstCaseAt" class="cell"><b>CM in-circuit @ f<sub>design</sub> (25 Ω LISN)</b>
            <span class="chip" :class="worstCaseAt.cm.standard >= Number(aReqCm) ? 'pass' : 'fail'" data-test="wc-verdict-cm">
              {{ fmtDb(worstCaseAt.cm.standard) }} dB {{ worstCaseAt.cm.standard >= Number(aReqCm) ? '≥' : '<' }} {{ fmtDb(Number(aReqCm), 0) }}</span>
          </div>
          <div v-if="worstCaseAt" class="cell"><b>DM in-circuit @ f<sub>design</sub> (100 Ω LISN)</b>
            <span class="chip" :class="worstCaseAt.dm.standard >= Number(aReqDm) ? 'pass' : 'fail'" data-test="wc-verdict-dm">
              {{ fmtDb(worstCaseAt.dm.standard) }} dB {{ worstCaseAt.dm.standard >= Number(aReqDm) ? '≥' : '<' }} {{ fmtDb(Number(aReqDm), 0) }}</span>
          </div>
        </div>
        <p v-if="escalated" class="note" data-test="escalated-note">
          The asymptote-sized parts missed the in-circuit criterion, so the selector escalated to
          larger candidates until it passed — the verdict above scores what was actually selected.</p>
        <p v-if="worstCaseAt" class="note">
          Robustness caveat — CISPR 17 worst case (0.1 Ω/100 Ω, both orientations, worst kept):
          CM {{ fmtDb(worstCaseAt.cm.worst) }} dB, DM {{ fmtDb(worstCaseAt.dm.worst) }} dB at
          f<sub>design</sub>. The 0.1 Ω-source orientation is NOT the LISN measurement condition
          (an AMN presents 25 Ω CM / 100 Ω DM by construction); it bounds sensitivity to an unknown
          converter-side impedance, and single-stage DM filters score near 0 dB against it by nature.</p>
      </div>

      <FilterSchematic :stages="design.stages" :labels="schematicLabels()" :bindings="bindings"
                       :selected="selectedRef" @select="selectedRef = $event" />

      <div v-if="selectedRef && recommendations()" class="panel" data-test="part-panel">
        <p class="section-label">Catalog parts for {{ selectedRef.replace('C_YL', 'C_Y (pair) ').replace('C_YN', 'C_Y (pair) ') }}
          — target {{ fmtSi(recommendations().target, kindOf(selectedRef) === 'cmc' ? 'H' : 'F') }}</p>
        <p v-if="recommendations().unavailable" class="note">Parts catalog not reachable from this deployment — bind manually via the BOM later, or design on values only.</p>
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
          against the datasheet before committing; chip-scale data-line chokes are never mains parts.</p>
        <p v-if="kindOf(selectedRef) === 'cmc'" class="note">
          "Meas. IL" is computed from the part's measured complex impedance against your Y network at the
          design frequency (SILENT-style selection). Parts offered "by measured curve" have no catalogued
          inductance — binding one keeps the designed values in the netlist; the measured overlay shows its true prediction.</p>
      </div>

      <div class="panel">
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
          <button class="ghost" data-test="download-cias" :disabled="!allBound()" @click="downloadCias()"
                  :title="allBound() ? 'Download the CIAS circuit brick' : 'Bind every component first — a CIAS with placeholder parts is never produced'">
            Download CIAS brick
          </button>
        </div>
      </div>

      <div class="panel">
        <p class="section-label">Common mode — choke against 2×C<sub>Y</sub></p>
        <table class="data">
          <tbody>
            <tr><td>L<sub>CM</sub> required</td><td>{{ fmtSi(design.lCmRequiredH, 'H') }}</td></tr>
            <tr><td>L<sub>CM</sub> selected</td><td data-test="lcm"><strong>{{ fmtSi(design.lCmSelectedH, 'H') }}</strong> (per stage)</td></tr>
            <tr><td>C<sub>Y</sub></td><td>2 × {{ fmtSi(design.cYPerLineF, 'F') }} per stage</td></tr>
            <tr><td>Sizing asymptote (ideal 40·n·log₁₀ estimate — the in-circuit chip above is the verdict)</td>
              <td data-test="il-cm">{{ fmtDb(design.attenuationCmDb) }} dB</td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel">
        <p class="section-label">Differential mode — leakage against C<sub>X</sub></p>
        <table class="data">
          <tbody>
            <tr><td>L<sub>DM</sub> (leakage)</td><td>{{ fmtSi(design.lDmH, 'H') }}</td></tr>
            <tr><td>C<sub>X</sub> required</td><td>{{ fmtSi(design.cXRequiredF, 'F') }}</td></tr>
            <tr><td>C<sub>X</sub> selected</td><td><strong>{{ fmtSi(design.cXSelectedF, 'F') }}</strong> (per stage)</td></tr>
            <tr><td>Sizing asymptote (ideal — see the in-circuit chip above)</td>
              <td>{{ fmtDb(design.attenuationDmDb) }} dB</td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel" v-if="lCmSource === 'catalog' && matchedParts().length">
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

      <div class="panel" v-if="ilCm && ilDm">
        <p class="section-label">In-circuit insertion loss — solid: nominal terminations · dashed: CISPR 17 worst case (0.1 Ω/100 Ω) · dot: your requirement</p>
        <LogChart :series="ilSeries()" :violations="requirementMarkers()" violation-label="your requirements (CM green, DM blue)" y-label="dB" :height="300" data-test="il-chart" />
        <p class="note">If the dashed worst-case curve still clears your requirement at the design frequency, termination uncertainty cannot eat the margin.</p>
        <p class="note">Ideal-element curves ignore self-resonance, ESL and winding capacitance —
          above a few MHz a real single-stage filter plateaus at 50–70 dB. Bind a part with a
          measured curve (dotted) for the honest high-frequency picture; measured curves currently
          cover Murata parts only.</p>
        <p v-if="measured" class="note" data-test="measured-note">
          Dotted: predicted with the <strong>measured impedance curve</strong> of {{ measured.mpn }}
          (complex Z, manufacturer data via the TAS catalog) — shown only over the measured frequency span.</p>
      </div>

      <div class="panel" v-if="interaction">
        <p class="section-label">Input-filter interaction (Middlebrook)</p>
        <div class="row">
          <label class="field"><span>Converter min. input voltage (V)</span>
            <input v-model.number="vInMin" type="number" @change="compute" /></label>
          <label class="field"><span>Input power (W)</span>
            <input v-model.number="pIn" type="number" @change="compute" /></label>
        </div>
        <table class="data">
          <tbody>
            <tr><td>Filter resonance</td><td>{{ fmtHz(interaction.resonanceHz) }}</td></tr>
            <tr><td>Characteristic impedance R₀ (= peak output impedance <em>with the damping network fitted</em>)</td>
              <td>{{ fmtSi(interaction.characteristicImpedanceOhm, 'Ω') }}</td></tr>
            <tr><td>Converter input impedance V²/P</td><td>{{ fmtSi(interaction.converterInputImpedanceOhm, 'Ω') }}</td></tr>
            <tr><td>Stability margin (assumes R<sub>d</sub>/C<sub>d</sub> below are fitted)</td>
              <td :class="interaction.marginDb >= 12 ? 'pos' : interaction.marginDb >= 6 ? '' : 'neg'" data-test="middlebrook-margin">
                {{ fmtDb(interaction.marginDb) }} dB
                <span class="note">(≥ 12 dB comfortable, &lt; 6 dB add damping: R<sub>d</sub> = {{ fmtSi(interaction.dampingResistorOhm, 'Ω') }},
                C<sub>d</sub> = {{ fmtSi(interaction.dampingCapacitorMinF, 'F') }}–{{ fmtSi(interaction.dampingCapacitorMaxF, 'F') }})</span>
              </td></tr>
            <tr><td class="note" colspan="2">Undamped, the LC section rings with a Q set by parasitic
              resistances this tool does not know — the true undamped peak can be 10–50× R₀. Fit the
              damping branch whenever the margin above is what keeps the converter stable.</td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel" v-if="design.leakageCurrentA !== undefined">
        <p class="section-label">Safety checks (worst case: V+10 %, C+20 %)</p>
        <table class="data">
          <tbody>
            <tr><td>PE touch current (single line-side Y path per IEC 60990 — worst case V+10 %, C+20 %)
                <select v-model.number="touchLimitMa" style="width: auto; margin-left: 0.5em" data-test="touch-tier">
                  <option :value="3.5">vs 3.5 mA (IEC 62368-1 Class I)</option>
                  <option :value="0.75">vs 0.75 mA (appliance)</option>
                  <option :value="0.5">vs 0.5 mA (medical)</option>
                </select></td>
              <td :class="design.leakageCurrentA < touchLimitMa * 1e-3 ? 'pos' : 'neg'" data-test="touch-verdict">
                {{ fmtSi(design.leakageCurrentA, 'A') }}
                {{ design.leakageCurrentA < touchLimitMa * 1e-3 ? 'passes' : 'EXCEEDS' }} {{ touchLimitMa }} mA</td></tr>
            <tr><td>X discharge resistor (V+10 %, C+20 %)</td>
              <td>≤ {{ fmtSi(design.dischargeResistorMaxOhm, 'Ω') }} → <strong>{{ fmtSi(design.dischargeResistorOhm, 'Ω') }}</strong>
                ({{ fmtSi(design.dischargeResistorPowerW, 'W') }} continuous — mind no-load standby budgets,
                and use a series pair or an HV-rated resistor: single chip parts are typically rated 150–200 V)</td></tr>
          </tbody>
        </table>
      </div>

      <div class="panel">
        <p class="section-label">SPICE export — filter + CISPR 16 LISN, ready for Kirchhoff / ngspice / LTspice</p>
        <label class="field"><span>Excitation deck</span>
          <select v-model="netlistMode" data-test="netlist-mode"
                  @change="api().then((e) => { try { netlist = e.filterSpiceNetlist(design, 'cispr16', netlistMode) } catch (deckError) { netlist = '* netlist unavailable: ' + deckError.message } })">
            <option value="dm">differential-mode drive (C_X path)</option>
            <option value="cm">common-mode drive (choke + Y caps)</option>
          </select></label>
        <pre class="code" data-test="netlist">{{ netlist }}</pre>
        <div class="row" style="margin-top: 0.6rem">
          <button class="ghost" @click="downloadNetlist">Download .cir</button>
          <button class="ghost" @click="copyNetlist">Copy</button>
        </div>
      </div>
    </div>
    <div v-else class="panel">
      <p class="note">Set the requirement (or bring one over from a failed scan on the Spectrum screen) and press <em>Design filter</em>. Candidate lists take any manufacturer's catalog values; paste your preferred vendor's series to design with real parts.</p>
    </div>
  </div>
</template>
